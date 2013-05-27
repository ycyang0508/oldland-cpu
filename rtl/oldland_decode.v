/*
 * Generate a variety of control signals from the input selection.  These
 * control signals include register selection, ALU opcode + operand selection
 * and load/store signals.
 *
 * Also perform the operand fetch for the case of registers.  So combinational
 * logic for the register fetch, everything else needs to be registered for
 * use in the execute cycle.
 */
module oldland_decode(input wire clk,
		      input wire [31:0] instr,
		      output wire [2:0] ra_sel,
		      output wire [2:0] rb_sel,
		      output reg [2:0] rd_sel,
		      output reg update_rd,
		      output reg [31:0] imm32,
		      output reg [3:0] alu_opc,
		      output reg [2:0] branch_condition,
		      output reg alu_op1_ra,
		      output reg alu_op2_rb,
		      output reg mem_load,
		      output reg mem_store,
		      output reg [1:0] mem_width,
		      output reg branch_ra,
		      input wire [31:0] pc_plus_4,
		      output reg [31:0] pc_plus_4_out,
		      output reg [1:0] instr_class,
		      output reg is_call);

wire [1:0] class = instr[31:30];
wire [3:0] opcode = instr[29:26];

/* Sign extended immediates. */
wire [31:0] imm16 = {{16{instr[25]}}, instr[25:10]};
wire [31:0] imm24 = {{6{instr[23]}}, instr[23:0], 2'b00};

/* For RET, we need to move the link register into the PC. */
assign ra_sel = class == `CLASS_BRANCH && opcode == `OPCODE_RET ? 3'd6 : instr[5:3];
assign rb_sel = instr[2:0];

initial begin
	alu_opc = 4'b0;
	update_rd = 1'b0;
	branch_condition = 3'b0;
	imm32 = 32'b0;
	branch_ra = 1'b0;
	alu_op1_ra = 1'b0;
	alu_op2_rb = 1'b0;
	mem_load = 1'b0;
	mem_store = 1'b0;
	mem_width = 1'b0;
	rd_sel = 3'b0;
	pc_plus_4_out = 32'b0;
	is_call = 1'b0;
	instr_class = 2'b00;
end

always @(posedge clk) begin
	branch_condition <= 3'b0;

	if (class == `CLASS_BRANCH) begin
		case (opcode)
		`OPCODE_BNE: branch_condition <= 3'b001;
		`OPCODE_BEQ: branch_condition <= 3'b010;
		`OPCODE_BGT: branch_condition <= 3'b011;
		`OPCODE_BLT: branch_condition <= 3'b100;
		default: branch_condition <= 3'b111;
		endcase
	end
end

always @(posedge clk) begin
	alu_opc <= class == `CLASS_ARITH ? opcode : 4'b0000;
	/*
	* Whether we store the result of the ALU operation in the destination
	* register or not.  This is almost all arithmetic operations apart from cmp
	* where we intentionally discard the result and load operations where the
	* register is update by the LSU later.
	*/
	update_rd <= class == `CLASS_ARITH && opcode != `OPCODE_CMP;

	/*
	* The output immediate - either one of the sign extended immediates or
	* a special case for movhi - the 16 bit immediate shifted left 16 bits.
	*/
	imm32 <= (class == `CLASS_BRANCH) ? imm24 :
		(opcode == `OPCODE_MOVHI) ? {instr[25:10], 16'b0} : imm16;

	branch_ra <= class == `CLASS_BRANCH && instr[25];

	alu_op1_ra <= (class == `CLASS_ARITH || class == `CLASS_MEM);
	alu_op2_rb <= (class == `CLASS_ARITH && instr[9]);

	mem_load <= class == `CLASS_MEM && instr[28] == 1'b0;
	mem_store <= class == `CLASS_MEM && instr[28] == 1'b1;

	rd_sel <= instr[8:6];
	instr_class <= class;

	is_call <= class == `CLASS_BRANCH && opcode == `OPCODE_CALL;
end

always @(posedge clk) begin
	case (instr[27:26])
	2'b00: mem_width <= 2'b10;
	2'b01: mem_width <= 2'b01;
	2'b10: mem_width <= 2'b00;
	default: mem_width <= 2'b00;
	endcase
end

/* Register the PC + 4 for PC relative accesses in the execute stage. */
always @(posedge clk) pc_plus_4_out <= pc_plus_4;

endmodule
