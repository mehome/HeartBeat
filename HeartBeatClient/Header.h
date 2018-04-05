#pragma once
// ���� ifdef ���Ǵ���ʹ�� DLL �������򵥵�
// ��ı�׼�������� DLL �е������ļ��������������϶���� IOCPAUTOUPDATE_EXPORTS
#define MAX_BUFFER_LEN        8192 
#include <cstdint>
typedef struct _tagHeader {
	uint32_t dwVer;
	uint32_t dwCmd;		// ��Ϣ����
	uint32_t dwLen;
}HEADER, *LPHEADER;// ����������Ϣ�Ľṹ��


#define NET_CMD_HEART_BEAT 0x00000001

#define NET_CMD_DATA 0x00000002
typedef struct _tagData {
	char szData[MAX_BUFFER_LEN];
}DATA, *LPDATA;

#define T1				60		/* idle time before heartbeat */
#define T2				10		/* time to wait for response */