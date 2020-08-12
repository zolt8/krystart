/*This code is part of the krystart Init System.
* The Krystart Init System is maintained by bzolt8
* The Krystart is a fork of Epoch by Subsentient.
* This software is public domain.
* Please read the file UNLICENSE.TXT for more information.*/

/**This file holds functions for the various things we can do, depending on argv[0].
 * Most are to be called by main() at some point or another.**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include "krystart.h"

/*To shut up some weird compilers. I don't know what this thing wants from me.*/
pid_t getsid(pid_t);

ReturnCode SendPowerControl(const char *MembusCode)
{ /*Client side to send a request to halt/reboot/power off/disable or enable CAD/etc.*/
	char InitsResponse[MEMBUS_MSGSIZE], *PCode[2], *PErrMsg;
	
	if (!strcmp(MembusCode, MEMBUS_CODE_HALT))
	{
		PCode[0] = MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_HALT;
		PCode[1] = MEMBUS_CODE_FAILURE " " MEMBUS_CODE_HALT;
		PErrMsg = "Unable to halt.";
	}
	else if (!strcmp(MembusCode, MEMBUS_CODE_POWEROFF))
	{
		PCode[0] = MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_POWEROFF;
		PCode[1] = MEMBUS_CODE_FAILURE " " MEMBUS_CODE_POWEROFF;
		PErrMsg = "Unable to power off.";
	}
	else if (!strcmp(MembusCode, MEMBUS_CODE_REBOOT))
	{
		PCode[0] = MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_REBOOT;
		PCode[1] = MEMBUS_CODE_FAILURE " " MEMBUS_CODE_REBOOT;
		PErrMsg = "Unable to reboot.";
	}
	else if (!strcmp(MembusCode, MEMBUS_CODE_CADON))
	{
		PCode[0] = MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_CADON;
		PCode[1] = MEMBUS_CODE_FAILURE " " MEMBUS_CODE_CADON;
		PErrMsg = "Unable to enable CTRL-ALT-DEL instant reboot.";
	}
	else if (!strcmp(MembusCode, MEMBUS_CODE_CADOFF))
	{
		PCode[0] = MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_CADOFF;
		PCode[1] = MEMBUS_CODE_FAILURE " " MEMBUS_CODE_CADOFF;
		PErrMsg = "Unable to disable CTRL-ALT-DEL instant reboot.";
	}
	else
	{
		SpitError("Invalid MEMBUS_CODE passed to SendPowerControl().");
		return FAILURE;
	}
	
	if (!MemBus_Write(MembusCode, false))
	{
		SpitError("Failed to write to membus.");
		return FAILURE;
	}
	
	while (!MemBus_Read(InitsResponse, false)) usleep(1000); /*0.001 secs.*/
	
	MemBus_Write(MembusCode, false); /*Tells init it can shut down the membus.*/
	
	if (!strcmp(InitsResponse, PCode[0]))
	{
		ShutdownMemBus(false);
		return SUCCESS;
	}
	else if (!strcmp(InitsResponse, PCode[1]))
	{ /*Nothing uses this right now.*/
		SpitError(PErrMsg);
		ShutdownMemBus(false);
		
		return FAILURE;
	}
	
	return SUCCESS;
}

ReturnCode ObjControl(const char *ObjectID, const char *MemBusSignal)
{ /*Start and stop or disable services.*/
	char RemoteResponse[MEMBUS_MSGSIZE];
	char OutMsg[MEMBUS_MSGSIZE];
	char PossibleResponses[4][MEMBUS_MSGSIZE];
	
	snprintf(OutMsg, sizeof OutMsg, "%s %s", MemBusSignal, ObjectID);
	
	if (!MemBus_Write(OutMsg, false))
	{
		return FAILURE;
	}
	
	while (!MemBus_Read(RemoteResponse, false)) usleep(1000); /*0.001 secs*/
	
	snprintf(PossibleResponses[0], sizeof PossibleResponses[0], "%s %s %s",
		MEMBUS_CODE_ACKNOWLEDGED, MemBusSignal, ObjectID);
		
	snprintf(PossibleResponses[1], sizeof PossibleResponses[1], "%s %s %s",
		MEMBUS_CODE_FAILURE, MemBusSignal, ObjectID);
		
	snprintf(PossibleResponses[2], sizeof PossibleResponses[2], "%s %s",
		MEMBUS_CODE_BADPARAM, OutMsg);

	snprintf(PossibleResponses[3], sizeof PossibleResponses[3], "%s %s %s",
		MEMBUS_CODE_WARNING, MemBusSignal, ObjectID);
		
	if (!strcmp(RemoteResponse, PossibleResponses[0]))
	{
		return SUCCESS;
	}
	else if (!strcmp(RemoteResponse, PossibleResponses[1]))
	{
		return FAILURE;
	}
	else if (!strcmp(RemoteResponse, PossibleResponses[3]))
	{
		return WARNING;
	}
	else if (!strcmp(RemoteResponse, PossibleResponses[2]))
	{
		SpitError("\nWe are being told that we sent a bad parameter.");
		return FAILURE;
	}
	else
	{
		SpitError("\nReceived invalid reply from membus.");
		return FAILURE;
	}
}

ReturnCode EmulKillall5(unsigned InSignal)
{ /*Used as the killall5 utility.*/
	DIR *ProcDir;
	struct dirent *CurDir;
	pid_t CurPID;
	const pid_t OurPID = getpid(), OurSID = getsid(0);

	if (InSignal > SIGSTOP || InSignal == 0) /*Won't be negative since we are unsigned.*/
	{
		SpitError("EmulKillall5() Bad value for unsigned InSignal.");
	}
	
	/*We get everything from /proc.*/
	if (!(ProcDir = opendir("/proc/")))
	{
		return FAILURE;
	}
	
	/*Stop everything.*/
	kill(-1, SIGSTOP);
	
	while ((CurDir = readdir(ProcDir)))
	{
		if (AllNumeric(CurDir->d_name) && CurDir->d_type == 4)
		{			
			CurPID = atol(CurDir->d_name); /*Convert the new PID to a true number.*/
			
			if (CurPID == 1 || CurPID == OurPID)
			{ /*Don't try to kill init, or us.*/
				continue;
			}
			
			
			if (getsid(CurPID) == OurSID)
			{ /*It's in our session ID, so don't touch it.*/
				continue;
			}
			
			/*We made it this far, must be safe to nuke this process.*/
			kill(CurPID, InSignal); /*Actually send the kill, stop, whatever signal.*/
		}
	}
	closedir(ProcDir);
	
	/*Start it up again.*/
	kill(-1, SIGCONT);
	
	return SUCCESS;
}

void EmulWall(const char *InStream, Bool ShowUser)
{ /*We not only use this as a CLI applet, we use it to notify of impending shutdown too.*/
	char OutBuf[8192];
	char HMS[3][16];
	char MDY[3][16];
	const char *OurUser = getenv("USER");
	char OurHostname[512] = { '\0' };
	DIR *DevDir;
	DIR *PtsDir;
	struct dirent *DirPtr;
	char FileNameBuf[MAX_LINE_SIZE];
	int FileDescriptor = 0;
	
	if (getuid() != 0)
	{ /*Not root?*/
		SpitWarning("You are not root. Only sending to ttys you have privileges on.");
	}
	
	GetCurrentTime(HMS[0], HMS[1], HMS[2], MDY[0], MDY[1], MDY[2]);
	
	snprintf(OutBuf, 64, "\007\n%s[%s:%s:%s | %s-%s-%s]%s ", CONSOLE_COLOR_RED, HMS[0], HMS[1], HMS[2],
		MDY[0], MDY[1], MDY[2], CONSOLE_ENDCOLOR);
	
	if (ShowUser)
	{
		int HostnameLen = 0;		
		if (gethostname(OurHostname, sizeof OurHostname / 2) != 0)
		{
			strncpy(OurHostname, "(unknown)", sizeof "(unknown)");
		}
		
		HostnameLen = strlen(OurHostname);
		
		if (getdomainname(OurHostname + HostnameLen + 1, sizeof OurHostname / 2 - 1) == 0 &&
			strcmp(OurHostname + HostnameLen + 1, "(none)") != 0 &&
			strcmp(OurHostname + HostnameLen + 1, "") != 0)
		{ /*If we DO have a domain name, set it.*/
			OurHostname[HostnameLen] = '.';
		}
			
		/*I really enjoy pulling stuff off like the line below.*/
		snprintf(&OutBuf[strlen(OutBuf)], sizeof OutBuf - strlen(OutBuf), "Broadcast message from %s@%s: ",
				(OurUser != NULL ? OurUser : "(unknown)"), OurHostname);
		
	}
	else
	{
		snprintf(&OutBuf[strlen(OutBuf)], sizeof OutBuf - strlen(OutBuf), "%s", "Broadcast message: ");
	}
	
	snprintf(&OutBuf[strlen(OutBuf)], sizeof OutBuf - strlen(OutBuf), "\n%s\n\n", InStream);
	if ((DevDir = opendir("/dev/")))
	{ /*Now write to the ttys.*/
		while ((DirPtr = readdir(DevDir))) /*See, we use fopen() as a way to check if the file exists.*/
		{ /*Your eyes bleeding yet?*/
			if (!strncmp(DirPtr->d_name, "tty", sizeof "tty" - 1) &&
				strlen(DirPtr->d_name) > sizeof "tty" - 1 &&
				isdigit(DirPtr->d_name[sizeof "tty" - 1]) &&
				atoi(DirPtr->d_name + sizeof "tty" - 1) > 0)
			{
				snprintf(FileNameBuf, MAX_LINE_SIZE, "/dev/%s", DirPtr->d_name);
				
				if ((FileDescriptor = open(FileNameBuf, O_WRONLY | O_NONBLOCK)) == -1)
				{ /*Screw it, we don't care.*/
					continue;
				}
				
				write(FileDescriptor, OutBuf, strlen(OutBuf));
				
				close(FileDescriptor); FileDescriptor = 0;
			}
		}
		closedir(DevDir);
	}
	
	if ((PtsDir = opendir("/dev/pts/")))
	{
		while ((DirPtr = readdir(PtsDir)))
		{
			if (isdigit(DirPtr->d_name[0]))
			{
				snprintf(FileNameBuf, MAX_LINE_SIZE, "/dev/pts/%s", DirPtr->d_name);
				
				if ((FileDescriptor = open(FileNameBuf, O_WRONLY | O_NONBLOCK)) == -1)
				{
					continue;
				}
				
				write(FileDescriptor, OutBuf, strlen(OutBuf));
				close(FileDescriptor); FileDescriptor = 0;
			}
		}
		closedir(PtsDir);
	}
}

ReturnCode EmulShutdown(int ArgumentCount, const char **ArgStream)
{ /*Eyesore, but it works.*/
	const char **TPtr = ArgStream + 1; /*Skip past the equivalent of argv[0].*/
	unsigned TargetHr = 0, TargetMin = 0;
	const char *THalt = NULL;
	char PossibleResponses[3][MEMBUS_MSGSIZE];
	char TmpBuf[MEMBUS_MSGSIZE], InRecv[MEMBUS_MSGSIZE], TimeFormat[32];
	short Inc = 0;
	short TimeIsSet = 0, HaltModeSet = 0;
	Bool AbortingShutdown = false, ImmediateHalt = false;
	
	for (; Inc != (ArgumentCount - 1); ++TPtr, ++Inc)
	{
		if (!strcmp(*TPtr, "-h") || !strcmp(*TPtr, "--halt") || !strcmp(*TPtr, "-H"))
		{
			THalt = MEMBUS_CODE_HALT;
			++HaltModeSet;
			continue;
		}
		else if (!strcmp(*TPtr, "-R") || !strcmp(*TPtr, "-r") || !strcmp(*TPtr, "--reboot"))
		{
			THalt = MEMBUS_CODE_REBOOT;
			++HaltModeSet;
			continue;
		}
		else if (!strcmp(*TPtr, "-p") || !strcmp(*TPtr, "-P") || !strcmp(*TPtr, "--poweroff"))
		{
			THalt = MEMBUS_CODE_POWEROFF;
			++HaltModeSet;
			continue;
		}
		else if (!strcmp(*TPtr, "-c") || !strcmp(*TPtr, "--cancel"))
		{
			AbortingShutdown = true;
			snprintf(TmpBuf, sizeof TmpBuf, "%s", MEMBUS_CODE_ABORTHALT);
			break;
		}
		else if (strchr(*TPtr, ':') && **TPtr != '-')
		{
			struct _HaltParams TempParams;
			
			if (sscanf(*TPtr, "%u:%u", &TargetHr, &TargetMin) != 2)
			{
				puts("Bad time format. Please enter in the format of \"hh:mm\"");
				return FAILURE;
			}
			
			DateDiff(TargetHr, TargetMin, &TempParams.TargetMonth, &TempParams.TargetDay, &TempParams.TargetYear);
			
			snprintf(TimeFormat, sizeof TimeFormat, "%u:%u:%d %u/%u/%u",
					TargetHr, TargetMin, 0, TempParams.TargetMonth, TempParams.TargetDay, TempParams.TargetYear);
					
			++TimeIsSet;
		}
		else if (**TPtr == '+' && AllNumeric(*TPtr + 1))
		{
			struct _HaltParams TempParams;
			const char *TArg = *TPtr + 1; /*Targ manure!*/
			time_t TTime;
			struct tm TimeStruct;
			
			MinsToDate(atoi(TArg), &TempParams.TargetHour, &TempParams.TargetMin, &TempParams.TargetMonth,
						&TempParams.TargetDay, &TempParams.TargetYear);
						
			time(&TTime); /*Get this for the second.*/
			localtime_r(&TTime, &TimeStruct);
			
			snprintf(TimeFormat, sizeof TimeFormat, "%u:%u:%d %u/%u/%u",
					TempParams.TargetHour, TempParams.TargetMin, (int)TimeStruct.tm_sec, TempParams.TargetMonth,
					TempParams.TargetDay, TempParams.TargetYear);
					
			++TimeIsSet;
		}
		else if (!strcmp(*TPtr, "now"))
		{
			ImmediateHalt = true;
			++TimeIsSet;
		}
		else if (!strcmp(*TPtr, "--help"))
		{
			const char *HelpMsg =
			"Usage: shutdown -hrpc [12:00/+10/now] -c\n\n"
			"-h -H --halt: Halt the system, don't power down.\n"
			"-p -P --poweroff: Power down the system.\n"
			"-r -R --reboot: Reboot the system.\n"
			"-c --cancel: Cancel a pending shutdown.\n\n"
			"Specify time in hh:mm, +m, or \"now\".\n";
			
			puts(HelpMsg);
			return SUCCESS;
		}
			
		else
		{
			fprintf(stderr, "Invalid argument %s. See --help for usage.\n", *TPtr);
			return FAILURE;
		}

	}
	
	if (!AbortingShutdown)
	{
		if (HaltModeSet == 0)
		{
			fprintf(stderr, "%s\n", "You must specify one of -hrp.");
			return FAILURE;
		}
		
		if (HaltModeSet > 1)
		{
			fprintf(stderr, "%s\n", "Please specify only ONE of -hrp.");
			return FAILURE;
		}
		
		if (!TimeIsSet)
		{
			fprintf(stderr, "%s\n", "You must specify a time in the format of hh:mm: or +m.");
			return FAILURE;
		}
		
		if (TimeIsSet > 1)
		{
			fprintf(stderr, "%s\n", "Multiple time arguments specified. Please specify only one.");
			return FAILURE;
		}
		
		if (!ImmediateHalt)
		{
			snprintf(TmpBuf, sizeof TmpBuf, "%s %s", THalt, TimeFormat);
		}
	}
	
	if (ImmediateHalt)
	{
		snprintf(TmpBuf, sizeof TmpBuf, "%s", THalt);
	}
	
	snprintf(PossibleResponses[0], MEMBUS_MSGSIZE, "%s %s", MEMBUS_CODE_ACKNOWLEDGED, TmpBuf);
	snprintf(PossibleResponses[1], MEMBUS_MSGSIZE, "%s %s", MEMBUS_CODE_FAILURE, TmpBuf);
	snprintf(PossibleResponses[2], MEMBUS_MSGSIZE, "%s %s", MEMBUS_CODE_BADPARAM, TmpBuf);
	
	if (!InitMemBus(false))
	{
		SpitError("Failed to connect to membus.");
		return FAILURE;
	}
	
	if (!MemBus_Write(TmpBuf, false))
	{
		SpitError("Failed to write to membus.");
		ShutdownMemBus(false);
		return FAILURE;
	}
	
	while (!MemBus_Read(InRecv, false)) usleep(1000); /*Wait for a response.*/
	
	if (ImmediateHalt) MemBus_Write(" ", false); /*Tells init it can shut down the membus.*/
	
	if (!ShutdownMemBus(false))
	{
		SpitError("Failed to shut down membus! This could spell serious issues.");
	}
	
	if (!strcmp(InRecv, PossibleResponses[0]))
	{
		return SUCCESS;
	}
	else if (!strcmp(InRecv, PossibleResponses[1]))
	{
		if (!strcmp(TmpBuf, MEMBUS_CODE_ABORTHALT))
		{
			fprintf(stderr, "%s\n", "Failed to abort shutdown. Is a shutdown scheduled?");
		}
		else
		{
			fprintf(stderr, "%s\n", "Failed to schedule shutdown.\nIs another already scheduled? Use shutdown -c to cancel it.");
		}
		return FAILURE;
	}
	else if (!strcmp(InRecv, PossibleResponses[2]))
	{
		SpitError("We are being told that we sent a bad parameter over the membus!"
					"Please report this to Krystart, as it's likely a bug.");
		return FAILURE;
	}
	else
	{
		SpitError("Invalid response received from membus! Please report this to Krystart, as it's likely a bug.");
		return FAILURE;
	}
}
