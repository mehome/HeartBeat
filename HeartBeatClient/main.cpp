// HeartBeatClient.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "ClientSocket.h"

int main()
{
	ClientSocket client;
	client.LoadSockLib();
	client.start();
	client.stop();
    return 0;
}

