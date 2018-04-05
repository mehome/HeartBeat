#pragma once
// 下列 ifdef 块是创建使从 DLL 导出更简单的
// 宏的标准方法。此 DLL 中的所有文件都是用命令行上定义的 IOCPAUTOUPDATE_EXPORTS
#define MAX_BUFFER_LEN        8192 
#include <cstdint>
typedef struct _tagHeader {
	uint32_t dwVer;
	uint32_t dwCmd;		// 消息类型
	uint32_t dwLen;
}HEADER, *LPHEADER;// 用来接收消息的结构体


#define NET_CMD_HEART_BEAT 0x00000001

#define NET_CMD_DATA 0x00000002
typedef struct _tagData {
	char szData[MAX_BUFFER_LEN];
}DATA, *LPDATA;

#define T1				60		/* idle time before heartbeat */
#define T2				10		/* time to wait for response */