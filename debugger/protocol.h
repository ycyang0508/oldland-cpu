#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

enum dbg_cmd {
	CMD_STOP,
	CMD_RUN,
	CMD_STEP,
	CMD_READ_REG,
	CMD_WRITE_REG,
	CMD_RMEM32,
	CMD_RMEM16,
	CMD_RMEM8,
	CMD_WMEM32,
	CMD_WMEM16,
	CMD_WMEM8,
	CMD_RESET,
	CMD_CACHE_SYNC,
	CMD_CPUID,
	CMD_GET_EXEC_STATUS,

	CMD_START_TRACE = -2,
	CMD_SIM_TERM = -1,
};

enum dbg_reg {
	REG_CMD,	/* Command register. */
	REG_ADDRESS,	/* Address register. */
	REG_WDATA,	/* Write data (write-only). */
	REG_RDATA,	/* Read data (read-only). */
};

struct dbg_request {
	uint32_t addr;
	uint32_t value;
	uint8_t read_not_write;
};

struct dbg_response {
	int32_t status;
	uint32_t data;
};

enum exec_status {
	EXEC_STATUS_RUNNING		= (1 << 0),
	EXEC_STATUS_STOPPED_ON_BKPT	= (1 << 1),
};

#endif /* __PROTOCOL_H__ */
