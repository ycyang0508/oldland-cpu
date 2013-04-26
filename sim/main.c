#define DEBUG

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <lua5.2/lua.h>
#include <lua5.2/lauxlib.h>
#include <lua5.2/lualib.h>

#include "internal.h"
#include "io.h"

enum sim_status {
	SIM_SUCCESS = 0xffffffff,
	SIM_FAIL = 0xfffffffe,
	SIM_CONTINUE = 0x00000000,
};

struct cpu {
	uint32_t pc;
	uint32_t next_pc;
	uint32_t regs[8];
	unsigned z:1;
	unsigned c:1;

	struct mem_map *mem;
	lua_State *lua_interp;
	FILE *trace_file;
};

static void trace(struct cpu *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(c->trace_file, fmt, ap);
	va_end(ap);
}

enum regs {
	R0, R1, R2, R3, R4, R5, FP, SP, PC
};

enum instruction_class {
	INSTR_ARITHMETIC,
	INSTR_BRANCH,
	INSTR_LDR_STR,
};

enum arith_opcode {
	ARITH_ADD	= 0x0,
	ARITH_ADDC	= 0x1,
	ARITH_SUB	= 0x2,
	ARITH_SUBC	= 0x3,
	ARITH_LSL	= 0x4,
	ARITH_LSR	= 0x5,
	ARITH_AND	= 0x6,
	ARITH_XOR	= 0x7,
	ARITH_BIC	= 0x8,
	ARITH_OR	= 0x9,
	ARITH_MOVHI	= 0xa,
};

enum branch_opcode {
	BRANCH_CALL	= 0x0,
	BRANCH_RET	= 0x1,
	BRANCH_B	= 0x4,
	BRANCH_BNE	= 0x5,
	BRANCH_BEQ	= 0x6,
	BRANCH_BGT	= 0x7,
};

enum ls_opcode {
	LS_LDR32	= 0x0,
	LS_LDR16	= 0x1,
	LS_LDR8		= 0x2,
	LS_STR32	= 0x4,
	LS_STR16	= 0x5,
	LS_STR8		= 0x6,
};

static inline enum instruction_class instr_class(uint32_t instr)
{
	return (instr >> 30) & 0x3;
}

static inline enum arith_opcode arith_opc(uint32_t instr)
{
	return (instr >> 26) & 0xf;
}

static inline enum branch_opcode branch_opc(uint32_t instr)
{
	return (instr >> 26) & 0xf;
}

static inline enum ls_opcode ls_opc(uint32_t instr)
{
	return (instr >> 26) & 0xf;
}

static inline enum regs instr_rd(uint32_t instr)
{
	return (instr >> 6) & 0x7;
}

static inline enum regs instr_ra(uint32_t instr)
{
	return (instr >> 3) & 0x7;
}

static inline enum regs instr_rb(uint32_t instr)
{
	return instr & 0x7;
}

static inline uint16_t instr_imm16(uint32_t instr)
{
	return (instr >> 10) & 0xffff;
}

static inline uint32_t instr_imm24(uint32_t instr)
{
	return instr & 0xffffff;
}

static void cpu_wr_reg(struct cpu *c, enum regs r, uint32_t v)
{
	trace(c, "[%08x] R%d := %08x\n", c->pc, r, v);
	c->regs[r] = v;
}

static void cpu_set_next_pc(struct cpu *c, uint32_t v)
{
	trace(c, "[%08x] PC := %08x\n", c->pc, v);
	c->next_pc = v;
}

static struct cpu *new_cpu(const char *romfile,
			   lua_State *lua_interp)
{
	int err;
	struct cpu *c = calloc(1, sizeof(*c));

	assert(c);
	c->mem = mem_map_new();
	assert(c->mem);

	err = ram_init(c->mem, 0x00000000, 0x10000, romfile);
	assert(!err);

	err = debug_uart_init(c->mem, 0x80000000, 0x1000);
	assert(!err);

	c->lua_interp = lua_interp;
	c->trace_file = fopen("oldland.trace", "w");
	assert(c->trace_file);

	return c;
}

static void emul_arithmetic(struct cpu *c, uint32_t instr)
{
	enum regs ra, rb, rd;
	uint16_t imm16;
	uint32_t op2;

	ra = instr_ra(instr);
	rb = instr_rb(instr);
	rd = instr_rd(instr);
	imm16 = instr_imm16(instr);
	op2 = (instr & (1 << 9)) ? c->regs[rb] : imm16;

	switch (arith_opc(instr)) {
	case ARITH_ADD:
		cpu_wr_reg(c, rd, c->regs[ra] + op2);
		break;
	case ARITH_SUB:
		cpu_wr_reg(c, rd, c->regs[ra] - op2);
		break;
	case ARITH_LSL:
		cpu_wr_reg(c, rd, c->regs[ra] << op2);
		break;
	case ARITH_LSR:
		cpu_wr_reg(c, rd, c->regs[ra] >> op2);
		break;
	case ARITH_AND:
		cpu_wr_reg(c, rd, c->regs[ra] & op2);
		break;
	case ARITH_XOR:
		cpu_wr_reg(c, rd, c->regs[ra] ^ op2);
		break;
	case ARITH_BIC:
		cpu_wr_reg(c, rd, c->regs[ra] & ~(1 << op2));
		break;
	case ARITH_OR:
		cpu_wr_reg(c, rd, c->regs[ra] | op2);
		break;
	case ARITH_MOVHI:
		cpu_wr_reg(c, rd, op2 << 16);
		break;
	default:
		die("invalid arithmetic opcode %u (%08x)\n", arith_opc(instr),
		    instr);
	}

	if (arith_opc(instr) != ARITH_MOVHI)
		c->z = !c->regs[rd];
}

static void emul_branch(struct cpu *c, uint32_t instr)
{
	enum regs rb = instr_rb(instr);
	int32_t imm24 = instr_imm24(instr);
	uint32_t target;

	/* Sign extend the immediate. */
	imm24 <<= 8;
	imm24 >>= 8;

	target = (instr & (1 << 25)) ? rb : c->pc + (imm24 << 2);

	switch (branch_opc(instr)) {
	case BRANCH_B:
		trace(c, "b\t%08x\n", target);
		cpu_set_next_pc(c, target);
		break;
	case BRANCH_BEQ:
		trace(c, "beq\t%08x\n", target);
		if (c->z)
			cpu_set_next_pc(c, target);
		break;
	default:
		die("invalid branch opcode %u (%08x)\n", branch_opc(instr),
		    instr);
	}
}

static void validate_result(struct cpu *c)
{
	lua_getglobal(c->lua_interp, "validate_result");
	if (!lua_isnil(c->lua_interp, -1))
		lua_call(c->lua_interp, 0, 0);
}

static int cpu_mem_map_write(struct cpu *c, physaddr_t addr,
			     unsigned int nr_bits, uint32_t val)
{
	lua_getglobal(c->lua_interp, "data_write_hook");
	if (!lua_isnil(c->lua_interp, -1)) {
		lua_pushinteger(c->lua_interp, addr);
		lua_pushinteger(c->lua_interp, nr_bits);
		lua_pushinteger(c->lua_interp, val);
		lua_call(c->lua_interp, 3, 0);
	} else {
		lua_pop(c->lua_interp, 1);
	}

	return mem_map_write(c->mem, addr, nr_bits, val);
}

static void emul_ldr_str(struct cpu *c, uint32_t instr)
{
	int32_t imm16 = instr_imm16(instr);
	uint32_t addr, v;
	enum regs ra = instr_ra(instr), rb = instr_rb(instr), rd = instr_rd(instr);
	int err;

	/* PC relative addressing. */
	if (instr & (1 << 9)) {
		/* Sign extend. */
		imm16 <<= 16;
		imm16 >>= 16;
		addr = c->pc + imm16;
	} else {
		addr = c->regs[ra] + imm16;
	}

	switch (ls_opc(instr)) {
	case LS_LDR8:
		err = mem_map_read(c->mem, addr, 8, &v);
		if (err)
			die("failed to read 8 bits @%08x\n", addr);
		cpu_wr_reg(c, rd, v & 0xff);
		break;
	case LS_STR8:
		v = c->regs[rb] & 0xff;
		err = cpu_mem_map_write(c, addr, 8, v);
		if (err)
			die("failed to write 8 bits @%08x\n", addr);
		break;
	default:
		die("invalid load/store opcode %u (%08x)\n", ls_opc(instr),
		    instr);
	}
}

static void emul_insn(struct cpu *c, uint32_t instr)
{
	switch (instr_class(instr)) {
	case INSTR_ARITHMETIC:
		emul_arithmetic(c, instr);
		break;
	case INSTR_BRANCH:
		emul_branch(c, instr);
		break;
	case INSTR_LDR_STR:
		emul_ldr_str(c, instr);
		break;
	default:
		die("invalid instruction class %u (%08x)\n",
		    instr_class(instr), instr);
	}
}

static uint32_t cpu_cycle(struct cpu *c)
{
	uint32_t instr;
	int err;

	trace(c, "------------------------------------------------------------------------\n");

	c->next_pc = c->pc + 4;

	err = mem_map_read(c->mem, c->pc, 32, &instr);
	assert(!err);

	if (instr == SIM_SUCCESS || instr == SIM_FAIL)
		return instr;

	emul_insn(c, instr);

	c->pc = c->next_pc;

	return SIM_CONTINUE;
}

static int lua_sim_err(lua_State *L)
{
	const char *msg = lua_tolstring(L, -1, NULL);

	die("%s\n", msg);

	return 0;
}

static const struct luaL_Reg sim_funcs[] = {
	{ "err", lua_sim_err },
	{}
};

static lua_State *init_test_script(const char *test_file)
{
	lua_State *L = luaL_newstate();

	assert(L);
	luaL_openlibs(L);
	luaL_newlib(L, sim_funcs);
	lua_setglobal(L, "sim");

	if (luaL_dofile(L, test_file))
		die("failed to load test %s (%s)\n", test_file,
		    lua_tostring(L, -1));

	return L;
}

int main(int argc, char *argv[])
{
	struct cpu *c;
	int err;
	lua_State *L;

	if (argc < 2)
		die("usage: %s TEST_FILE\n", argv[0]);

	L = init_test_script(argv[1]);

	c = new_cpu("rom.bin", L);
	printf("Oldland CPU simulator\n");

	do {
		err = cpu_cycle(c);
	} while (err == 0);

	printf("[%s]\n", err == SIM_SUCCESS ? "SUCCESS" : "FAIL");
	if (err == SIM_SUCCESS)
		validate_result(c);

	lua_close(L);

	return err == SIM_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}
