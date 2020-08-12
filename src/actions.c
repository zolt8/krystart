/*This code is part of the krystart Init System.
* The Krystart Init System is maintained by bzolt8
* The Krystart is a fork of Epoch by Subsentient.
* This software is public domain.
* Please read the file UNLICENSE.TXT for more information.*/

/**Handles bootup, shutdown, poweroff and reboot, etc, and some misc stuff.**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <signal.h>
#include "krystart.h"

/*Prototypes.*/
static void MountVirtuals(void);
static void PrimaryLoop(void);
static void ApplyGlobalEnvVars(void);

/*Globals.*/
struct _HaltParams HaltParams = { -1 };
unsigned char AutoMountOpts[5];
static Bool ContinuePrimaryLoop = true;
struct _EnvVarList *GlobalEnvVars;

/*Functions.*/
static void MountVirtuals(void)
{
	enum { MVIRT_PROC, MVIRT_SYSFS, MVIRT_DEVFS, MVIRT_PTS, MVIRT_SHM };
	
	const char *FSTypes[5] = { "proc", "sysfs", "devtmpfs", "devpts", "tmpfs" };
	const char *MountLocations[5] = { "/proc", "/sys", "/dev", "/dev/pts", "/dev/shm" };
	const char *PTSArg = "gid=5,mode=620";
	Bool HeavyPermissions[5] = { true, true, true, true, false };
	mode_t PermissionSet[2] = { (S_IRWXU | S_IRWXG | S_IRWXO), (S_IRWXU | S_IRGRP | S_IXGRP | S_IXOTH) };
	short Inc = 0;
	
	for (; Inc < 5; ++Inc)
	{
		if (AutoMountOpts[Inc])
		{
			if (AutoMountOpts[Inc] & MOUNTVIRTUAL_MKDIR)
			{ /*If we need to create a directory, do it.*/
				if (mkdir(MountLocations[Inc], PermissionSet[HeavyPermissions[Inc]] != 0))
				{
					char TmpBuf[1024];
					
					snprintf(TmpBuf, sizeof TmpBuf, "Failed to create directory for %s!", MountLocations[Inc]);
					WriteLogLine(TmpBuf, true);
					if (!(AutoMountOpts[Inc] & MOUNTVIRTUAL_NOERROR))
					{
						SpitWarning(TmpBuf);
					}
				} /*No continue statement because it might already exist*/
			}	/*and we might be able to mount it anyways.*/
			
			if (mount(FSTypes[Inc], MountLocations[Inc], FSTypes[Inc], 0, (Inc == MVIRT_PTS ? PTSArg : NULL)) != 0)
			{
				char TmpBuf[1024];

				if (AutoMountOpts[Inc] & MOUNTVIRTUAL_NOERROR)
				{
					snprintf(TmpBuf, sizeof TmpBuf, "Failed to mount virtual filesystem %s, but not an error.", MountLocations[Inc]);
					WriteLogLine(TmpBuf, true);
				}
				else
				{
					snprintf(TmpBuf, sizeof TmpBuf, "Failed to mount virtual filesystem %s!", MountLocations[Inc]);
					SpitWarning(TmpBuf);
					WriteLogLine(TmpBuf, true);
				}
				continue;
			}
			else
			{
				char TmpBuf[1024];
				
				snprintf(TmpBuf, sizeof TmpBuf, "Mounted virtual filesystem %s", MountLocations[Inc]);
				WriteLogLine(TmpBuf, true);
			}
		}
		
	}
}

static void PrimaryLoop(void)
{ /*Loop that provides essentially everything we cycle through.*/
	unsigned CurMin = 0, CurSec = 0;
	ObjTable *Worker = NULL;
	struct tm TimeStruct;
	time_t TimeCore;
	short LoopStepper = 0, ScanStepper = 0;
	
	for (ContinuePrimaryLoop = true; ContinuePrimaryLoop; ++LoopStepper)
	{	
	
		/**The line below is of critical importance. It harvests
		 * the zombies created by all processes throughout the system.**/
		while (waitpid(-1, NULL, WNOHANG) > 0);
		
		/*Do not flood the system with this big loop more than necessary.*/
		if (LoopStepper == 5)
		{
			
			LoopStepper = 0;
			
			HandleMemBusPings(); /*Tell clients we are alive if they ask.*/
			
			CheckMemBusIntegrity(); /*See if we need to manually disconnect a dead client.*/
			
			ParseMemBus(); /*Check membus for new data.*/
			
			if (HaltParams.HaltMode != -1)
			{
				time(&TimeCore);
				localtime_r(&TimeCore, &TimeStruct);
				
				CurMin = TimeStruct.tm_min;
				CurSec = TimeStruct.tm_sec;
				
				/*Allow a membus job to finish before shutdown, but actually do the shutdown afterwards.*/
				if (GetStateOfTime(HaltParams.TargetHour, HaltParams.TargetMin, HaltParams.TargetSec,
						HaltParams.TargetMonth, HaltParams.TargetDay, HaltParams.TargetYear))
				{ /*GetStateOfTime() returns 1 if the passed time is the present, and 2 if it's the past,
					so we can just take whatever positive value we are given.*/		
					LaunchShutdown(HaltParams.HaltMode);
				}
				else if (CurSec >= HaltParams.TargetSec && CurMin != HaltParams.TargetMin &&
					*DateDiff(HaltParams.TargetHour, HaltParams.TargetMin, NULL, NULL, NULL) <= 20 )
				{ /*If 20 minutes or less until shutdown, warn us every minute.*/
					char TBuf[MAX_LINE_SIZE];
					const char *HaltMode = NULL;
					static unsigned LastJobID = 0;
					static short LastMin = -1;
	
					if (LastJobID != HaltParams.JobID || CurMin != LastMin)
					{ /*Don't repeat ourselves 80 times while the second rolls over.*/
						const unsigned *const TimeReport = DateDiff(HaltParams.TargetHour, HaltParams.TargetMin, NULL, NULL, NULL);
						
						if (HaltParams.HaltMode == OSCTL_HALT)
						{
							HaltMode = "halt";
						}
						else if (HaltParams.HaltMode == OSCTL_POWEROFF)
						{
							HaltMode = "poweroff";
						}
						else
						{
							HaltParams.HaltMode = OSCTL_REBOOT;
							HaltMode = "reboot";
						}
						
						snprintf(TBuf, sizeof TBuf, "System is going down for %s in %u minutes %u seconds!",
								HaltMode, TimeReport[0], TimeReport[1]);
						EmulWall(TBuf, false);
						
						LastJobID = HaltParams.JobID;
						LastMin = CurMin;
					}
				}
			}
			
			if (ObjectTable)
			{
				for (Worker = ObjectTable; Worker->Next != NULL; Worker = Worker->Next)
				{ /*Handle objects intended for automatic restart.*/
					if (Worker->Opts.AutoRestart && Worker->Started && !ObjectProcessRunning(Worker))
					{
						char TmpBuf[MAX_LINE_SIZE];
						
						if (!Worker->Opts.HasPIDFile && AdvancedPIDFind(Worker, true))
						{ /* Try to update the PID rather than restart, since some things change their PIDs via forking etc.*/
							continue;
						}
						
						/*Don't let us enter a restart loop.*/
						if (Worker->StartedSince + (Worker->Opts.AutoRestart >> 1) > time(NULL))
						{
							snprintf(TmpBuf, sizeof TmpBuf,
									"AUTORESTART: "CONSOLE_COLOR_RED "PROBLEM:\n"
									"Object %s is trying to autorestart "
									"within 5 secs of last start.\n ** " CONSOLE_ENDCOLOR
									"Marking object stopped to safeguard against restart loop.",
									Worker->ObjectID);
									
							WriteLogLine(TmpBuf, true);
							
							Worker->Started = false;
							Worker->ObjectPID = 0;
							Worker->StartedSince = 0;
							continue;
						}
						
						snprintf(TmpBuf, MAX_LINE_SIZE, "AUTORESTART: Object %s is not running. Restarting.", Worker->ObjectID);
						WriteLogLine(TmpBuf, true);
						
						if (ProcessConfigObject(Worker, true, false))
						{
							snprintf(TmpBuf, MAX_LINE_SIZE, "AUTORESTART: Object %s successfully restarted.", Worker->ObjectID);
						}
						else
						{
							snprintf(TmpBuf, MAX_LINE_SIZE, "AUTORESTART: " CONSOLE_COLOR_RED "Failed" CONSOLE_ENDCOLOR
									" to restart object %s automatically.\nMarking object stopped.", Worker->ObjectID);
							Worker->Started = false;
							Worker->ObjectPID = 0;
							Worker->StartedSince = 0;
						}
						
						WriteLogLine(TmpBuf, true);
					}
					
					/*Rescan PIDs every minute to keep them up-to-date.*/
					if (ScanStepper == 240 && Worker->Started && !Worker->Opts.HasPIDFile)
					{
						AdvancedPIDFind(Worker, true);
					}
				}
			}
			
			if (ScanStepper == 240)
			{
				ScanStepper = 0;
			}
			
			++ScanStepper;
		}
		
		usleep(50000); /*0.05 secs*/

		/*Lots of brilliant code here, but I typed it in invisible pixels.*/
	}		
}

/*This does what it sounds like. It exits us to go to a shell in event of catastrophe.*/
void EmergencyShell(void)
{
	fprintf(stderr, CONSOLE_COLOR_MAGENTA "\nPreparing to start emergency shell." CONSOLE_ENDCOLOR "\n---\n");
	
	fprintf(stderr, "\nSyncing disks...\n");
	sync(); /*First things first, sync disks.*/
	
	fprintf(stderr, "Shutting down Krystart...\n");
	ShutdownConfig(); /*Release all memory.*/
	ShutdownMemBus(true); /*Stop the membus.*/
	
	fprintf(stderr, "Launching the shell...\n");
	
	execlp("sh", "sh", NULL); /*Nuke our process image and replace with a shell. No point forking.*/
	
	/*We're supposed to be gone! Something went wrong!*/
	SpitError("Failed to start emergency shell! Sleeping forever.");
	
	while (1) sleep(1); /*Hang forever to prevent a kernel panic.*/
}

void RecoverFromReexec(Bool ViaMemBus)
{ /*This is called when we are reexecuted from ReexecuteKrystart() to receive data*/
	pid_t ChildPID = 0;
	ObjTable *CurObj = NULL;
	char InBuf[MEMBUS_MSGSIZE] = { '\0' };
	char *MCode = MEMBUS_CODE_RXD;
	unsigned MCodeLength = strlen(MCode) + 1;
	short HPS = 0;
	unsigned TInc = 0;
	unsigned long OurLong; /*We write unsigned int values as unsigned long to maintan compatibility with 1.1.1 and earlier.*/
	MemBusKey = MEMKEY + 1;
	
	/*Restore any goobled up environ vars.*/
	setenv("USER", ENVVAR_USER, true);
	setenv("PATH", ENVVAR_PATH, true);
	setenv("HOME", ENVVAR_HOME, true);
	setenv("SHELL", ENVVAR_SHELL, true);
	
	if (!InitConfig(ConfigFile))
	{
		EmulWall("Krystart: "CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
		"Cannot reload configuration for re-exec!", false);
		EmergencyShell();
	}

	ApplyGlobalEnvVars(); /*Set global environment variables.*/
	
	if (!InitMemBus(false))
	{
		EmulWall("Krystart: "CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
				"Re-executed process cannot connect to modified membus.", false);
		EmergencyShell();
	}
	
	while (!MemBus_BinRead(InBuf, sizeof InBuf, false)) usleep(100);
	
	memcpy(&ChildPID, InBuf + MCodeLength, sizeof(pid_t));
	
	while (!MemBus_BinRead(InBuf, sizeof InBuf, false)) usleep(100);
	
	while (!strcmp(InBuf, MCode))
	{
		if ((CurObj = LookupObjectInTable(InBuf + MCodeLength)) != NULL)
		{
			unsigned TLength = strlen(CurObj->ObjectID) + 1;
			

			memcpy(&OurLong, (InBuf + MCodeLength + TLength), sizeof(long));
			CurObj->ObjectPID = OurLong;
			
			memcpy(&OurLong, (InBuf + MCodeLength + TLength + sizeof(long)), sizeof(Bool));
			CurObj->Started = OurLong;
			
			memcpy(&OurLong, (InBuf + MCodeLength + TLength + sizeof(long) + sizeof(Bool)), sizeof(long));
			CurObj->StartedSince = OurLong;
		}
		
		while (!MemBus_BinRead(InBuf, sizeof InBuf, false)) usleep(100);
	}
	
	MCode = MEMBUS_CODE_RXD_OPTS;
	MCodeLength = strlen(MCode) + 1;
	
	/*Retrieve the HaltParams structure.*/
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.HaltMode = *(long*)&OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetHour = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetMin = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetSec = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetMonth = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetDay = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.TargetYear = OurLong;
	
	memcpy(&OurLong, InBuf + MCodeLength + (HPS++ * sizeof(long)), sizeof(long));
	HaltParams.JobID = OurLong;
	
	/*Retrieve our important options.*/
	while (!MemBus_BinRead(InBuf, sizeof InBuf, false)) usleep(100);
	EnableLogging = (Bool)*(InBuf + MCodeLength);

	/*Retrieve the current runlevel.*/
	while (!MemBus_BinRead(InBuf, sizeof InBuf, false)) usleep(100);
	snprintf(CurRunlevel, sizeof CurRunlevel, "%s", InBuf + MCodeLength);
	
	MemBus_Write(MCode, false); /*Tell the child they can quit.*/
	
	/*Wait for the child to terminate.*/
	waitpid(ChildPID, NULL, 0); /*We don't really have to, but I think we should.*/
	
	/**
	 * EVERYTHING BEYOND HERE IS USED TO RESUME NORMAL OPERATION!
	 * **/
	
	/*Bring down the old, custom membus and bring up the classic.*/
	ShutdownMemBus(false);
	MemBusKey = MEMKEY;
	
	/*Reset environment variables.*/
	setenv("USER", ENVVAR_USER, true);
	setenv("PATH", ENVVAR_PATH, true);
	setenv("HOME", ENVVAR_HOME, true);
	setenv("SHELL", ENVVAR_SHELL, true);
	
	if (!InitMemBus(true))
	{
		SpitWarning("Cannot restart normal membus after re-exec. System is otherwise operational.");
	}
	
	if (ViaMemBus) /*Client probably wants confirmation.*/
	{
		/*Handle pings.*/
		for (; !HandleMemBusPings() && TInc < 100000; ++TInc)
		{ /*Wait ten seconds.*/
			 usleep(100);
		}
		
		if (TInc < 100000)
		{ /*Do not attempt this if we didn't receive a ping because it will only slow us down
			with another ten second timeout.*/
			/*Tell the client we are done.*/
			MemBus_Write(MEMBUS_CODE_ACKNOWLEDGED " " MEMBUS_CODE_RXD, true);
		}
	}
		
	FinaliseLogStartup(false); /*Bring back logging.*/
	LogInMemory = false;
	
	WriteLogLine(CONSOLE_COLOR_GREEN "Re-executed Krystart.\nNow using " VERSIONSTRING
				"\nCompiled " __DATE__ " " __TIME__ "." CONSOLE_ENDCOLOR, true);
				
	PrimaryLoop(); /*Does everything until the end of time.*/
}

void ReexecuteKrystart(void)
{ /*Used when Krystart needs to be restarted after we already booted.*/
	pid_t PID = 0;
	FILE *TestDescriptor = fopen(KRYSTART_BINARY_PATH, "rb");
	char OutBuf[MEMBUS_MSGSIZE] = { '\0' };
	ObjTable *Worker = ObjectTable;
	const char *MCode = MEMBUS_CODE_RXD;
	unsigned MCodeLength = strlen(MCode) + 1;
	short HPS = 0;
	unsigned long OurLong; /*Compatibility with 1.1.1 and earlier.*/
	
	
	ShutdownMemBus(true); /*We are now going to use a different MemBus key.*/
	MemBusKey = MEMKEY + 1; /*This prevents clients from interfering.*/
		
	
	if (!TestDescriptor)
	{
		EmulWall("Krystart: " CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
				"Unable to read \"" KRYSTART_BINARY_PATH "\"! Cannot reexec!", false);
	
		MemBusKey = MEMKEY;
		
		if (!InitMemBus(true))
		{
			SpitError("ReexecuteKrystart(): Failed to restart membus after failed reexec!");
		}
		
		MemBus_Write(MEMBUS_CODE_FAILURE " " MEMBUS_CODE_RXD, true);
		return;
	}
	else
	{
		fclose(TestDescriptor);
	}
	
	if ((PID = fork()) == -1)
	{
		EmulWall("Krystart: " CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
				"Unable to fork! Aborting reexecution.", false);
				
		MemBusKey = MEMKEY;
		
		if (!InitMemBus(true)) /*Bring back the membus.*/
		{
			SpitError("ReexecuteKrystart(): Failed to restart membus after failed reexec!");
		}
		
		MemBus_Write(MEMBUS_CODE_FAILURE " " MEMBUS_CODE_RXD, true);
		return;
	}
	
	/*Handle us if we are the original process first.*/
	if (PID > 0)
	{
		WriteLogLine(CONSOLE_COLOR_YELLOW "Re-executing Krystart..." CONSOLE_ENDCOLOR, true);
		
		while (shmget(MEMKEY + 1, MEMBUS_SIZE, 0660) == -1) usleep(100);
		
		/**Execute the new binary.**/ /*We pass the custom args to tell us we are re-executing.*/
		execlp(KRYSTART_BINARY_PATH, "!rxd", "REEXEC", NULL);
		
		/*Not supposed to be here.*/
		EmulWall(CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
				"Failed to execute \"" KRYSTART_BINARY_PATH "\"! Cannot reexec!", false);
				
		WriteLogLine(CONSOLE_COLOR_RED "Reexecution failed." CONSOLE_ENDCOLOR, true);
		kill(PID, SIGKILL); /*Kill the failed child.*/
		
		if (shmget(MEMKEY + 1, MEMBUS_SIZE, 0660) != -1)
		{
			ShutdownMemBus(true);
		}
		
		MemBusKey = MEMKEY;
		
		if (!InitMemBus(true))
		{
			SpitError("ReexecuteKrystart(): Failed to restart membus after failed reexec!");
		}
		
		MemBus_Write(MEMBUS_CODE_FAILURE " " MEMBUS_CODE_RXD, true);
		return;
	}
	
	/**The child is responsible for sending us our data.**/
	if (!InitMemBus(true))
	{
		EmulWall("Krystart: " CONSOLE_COLOR_RED "ERROR: " CONSOLE_ENDCOLOR
				"Re-exec: Child unable to start modified membus. Child terminating.", false);
		exit(1);
	}
	
	while (!HandleMemBusPings())
	{ /*Wait for the re-executed parent process to connect to receive it's config.*/
		static unsigned Counter = 0;
	
		usleep(1000); /*0.001 seconds.*/
		
		if (Counter == 0) ++Counter;
		else if (Counter >= 10000) /*Sleep ten seconds.*/
		{
			/*Quietly exit.*/
			SpitError("Host process not responding for reexec data exchange. Child exiting.");
			ShutdownMemBus(true);
			
			exit(1);
		}
	}
	
	/*Send our PID. This doubles as a greeting.*/
	PID = getpid();
	MemBus_BinWrite(&PID, sizeof(pid_t), true);
	
	strncpy(OutBuf, MCode, MCodeLength); /*We only need to do this once per MCode change, since we never wipe OutBuf.*/
	
	/*PIDs and started states.
	 * It doesn't matter if they are done eating the PID,
	 * MemBus_*Write() blocks until they're done with the first message.*/
	for (; Worker->Next; Worker = Worker->Next)
	{
		unsigned TLength = 0;
		strncpy(OutBuf + MCodeLength, Worker->ObjectID, (TLength = strlen(Worker->ObjectID) + 1));
		
		OurLong = Worker->ObjectPID;
		memcpy(OutBuf + MCodeLength + TLength, &OurLong, sizeof(long));
		
		OurLong = Worker->Started;
		memcpy(OutBuf + sizeof(long) + TLength + MCodeLength, &OurLong, sizeof(Bool));
		
		OurLong = Worker->StartedSince;
		memcpy(OutBuf + sizeof(long) + sizeof(Bool) + TLength + MCodeLength, &OurLong, sizeof(long));
		
		MemBus_BinWrite(OutBuf, sizeof OutBuf, true);
	}
	
	/*We change to a new code to mark the end of our loop.*/
	strncpy(OutBuf, (MCode = MEMBUS_CODE_RXD_OPTS), (MCodeLength = strlen(MEMBUS_CODE_RXD_OPTS) + 1));
	
	/*HaltParams, we're lazy and just write the whole structure.*/
	OurLong = HaltParams.HaltMode;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetHour;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetMin;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetSec;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetMonth;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetDay;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.TargetYear;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	OurLong = HaltParams.JobID;
	memcpy(OutBuf + MCodeLength + (HPS++ * sizeof(long)), &OurLong, sizeof HaltParams);
	
	MemBus_BinWrite(OutBuf, sizeof HaltParams + MCodeLength, true);
	
	/*Misc. global options. We don't include all because only some are used after initial boot.*/
	*(OutBuf + MCodeLength) = (char)EnableLogging;
	MemBus_BinWrite(OutBuf, MCodeLength + 1, true);
	
	/*The current runlevel is very important.*/
	strncpy(OutBuf + MCodeLength, CurRunlevel, strlen(CurRunlevel) + 1);
	MemBus_BinWrite(OutBuf, sizeof OutBuf, true);
	
	while (!MemBus_Read(OutBuf, true)) usleep(100); /*Wait for the main process to say we can quit.*/
	ShutdownMemBus(true); /*Nothing is deleted until the new process releases the lock, don't worry.*/
	ShutdownConfig();
	
	exit(0);
}

void PerformExec(const char *Cmd)
{
	unsigned Inc = 0, NumSpaces = 1, Inc2 = 0;
	char **Buffer = NULL;
	const char *Worker = Cmd;

	if (!Cmd)
	{ /*Some dipwad gave us a null pointer.*/
		const char *ErrMsg ="NULL value passed to PerformExec()! This is likely a bug. Please report.";
		SpitError(ErrMsg);
		WriteLogLine(ErrMsg, true);
		return;
	}
	
	/*Get the number of words in the command.*/
	while ((Worker = WhitespaceArg(Worker))) ++NumSpaces;
	
	/*Allocate space for pointers to represent each word.*/
	Buffer = malloc(sizeof(char*) * NumSpaces + 1);
	
	for (Worker = Cmd, Inc = 0; Inc < NumSpaces && Worker != NULL; ++Inc)
	{
		/*Count the length of this section of the command.*/
		for (Inc2 = 0; Worker[Inc2] != ' ' && Worker[Inc2] != '\t' && Worker[Inc2] != '\0'; ++Inc2);
		
		/*Allocate space for it.*/
		Buffer[Inc] = malloc(Inc2 + 1);
		
		/*Copy it into its Buffer cell.*/
		for (Inc2 = 0; Worker[Inc2] != ' ' && Worker[Inc2] != '\t' && Worker[Inc2] != '\0'; ++Inc2)
		{
			Buffer[Inc][Inc2] = Worker[Inc2];
		}
		Buffer[Inc][Inc2] = '\0';
		
		/*Skip to the next word in the command.*/
		Worker = WhitespaceArg(Worker);
	}
	
	/*Fill the last cell with nothing, as mandated by execvp().*/
	Buffer[NumSpaces] = NULL;
	
	sync(); /*Sync disks.*/
	
	ShutdownMemBus(true); /*Shutdown membus since we won't need it anymore.*/

	for (Inc = 1; Inc < NSIG; ++Inc)
	{ /*Reset signal handlers.*/
		signal(Inc, SIG_DFL);
	}

	execvp(Buffer[0], Buffer); /*Perform the exec.*/
	
	/**We should not still be here at this point.**/
	SpitError("exec() failed! Starting emergency shell.");
	EmergencyShell();
}

void PerformPivotRoot(const char *NewRoot, const char *OldRootDir)
{ /*Switch to a new root fs.*/
	if (!NewRoot || !OldRootDir)
	{ /*Safety first.*/
		if (NewRoot == NULL)
		{
			const char *ErrMsg = "NULL NewRoot passed to PerformPivotRoot()! This is likely a bug, please report.";
			SpitError(ErrMsg);
			WriteLogLine(ErrMsg, true);
		}
		
		if (OldRootDir == NULL)
		{
			const char *ErrMsg = "NULL OldRootDir passed to PerformPivotRoot()! This is likely a bug, please report.";
			SpitError(ErrMsg);
			WriteLogLine(ErrMsg, true);
		}
		
		return;
	}
	
	/*Sync to be safe.*/
	sync();

	/*pivot_root now.*/
	if (syscall(SYS_pivot_root, NewRoot, OldRootDir) != 0)
	{
		SpitError("Failed to pivot_root()!");
		EmergencyShell();
	}

	chdir("/"); /*Reset working directory*/
}


void FinaliseLogStartup(Bool BlankLog)
{
	if (MemLogBuffer != NULL)
	{ /*Switch logging out of memory mode and write it's memory buffer to disk.*/		
		if (EnableLogging)
		{
			if(strlen(LogFile) > 1)
			{

				FILE *Descriptor = fopen(LogFile, (BlankLog ? "w" : "a"));

				LogInMemory = false;

				if (!Descriptor)
				{
					SpitWarning("Cannot record logs to disk. Shutting down logging.");
					EnableLogging = false;
				}
				else
				{
					fwrite(MemLogBuffer, 1, strlen(MemLogBuffer), Descriptor);
					fflush(Descriptor);
					fclose(Descriptor);
				}
			}
			
		}
		
		free(MemLogBuffer); /*Release the memory anyways.*/
		MemLogBuffer = NULL;
	}
}

void LaunchBootup(void)
{ /*Handles what would happen if we were PID 1.*/
	
	setsid();
	
	/*Print our version to the console*/
	puts(CONSOLE_COLOR_CYAN VERSIONSTRING CONSOLE_ENDCOLOR);
	
	/*Set environment variables.*/
	setenv("USER", ENVVAR_USER, true);
	setenv("PATH", ENVVAR_PATH, true);
	setenv("HOME", ENVVAR_HOME, true);
	setenv("SHELL", ENVVAR_SHELL, true);
		
	/*Add tiny message if we passed krystartconfig= on the kernel cli.*/
	if (strcmp(ConfigFile, CONFIGDIR CONF_NAME) != 0)
	{
		printf("Using configuration file \"%s\".\n\n", ConfigFile);
	}
	
	/*Add tiny message if we passed runlevel= on the kernel cli.*/
	if (*CurRunlevel != '\0')
	{
		printf("Booting to runlevel \"%s\".\n\n", CurRunlevel);
	}

	if (*StartupCustomObjCommands.Start[0])
	{
		printf("Objects specified to start if found: ");
		
		int Inc = 0;
		char (*Arr)[sizeof *StartupCustomObjCommands.Start] = StartupCustomObjCommands.Start;
		
		for (; *Arr[Inc]; ++Inc)
		{
			printf("%s ", Arr[Inc]);
		}
		
		//Whitespace
		putchar('\n'); putchar('\n');
	}
	
	if (*StartupCustomObjCommands.Skip[0])
	{
		printf("Objects specified to skip if found: ");
		
		int Inc = 0;
		char (*Arr)[sizeof *StartupCustomObjCommands.Skip] = StartupCustomObjCommands.Skip;
		
		for (; *Arr[Inc]; ++Inc)
		{
			printf("%s ", Arr[Inc]);
		}
		
		//Whitespace
		putchar('\n'); putchar('\n');
	}
	
	if (InteractiveBoot)
	{
		const char *const Msg = "Booting in interactive mode.\n";
		puts(Msg);
		WriteLogLine(Msg, true);
	}
	
	if (!InitConfig(ConfigFile))
	{ /*That is very very bad if we fail here.*/
		EmergencyShell();
	}
	
	ApplyGlobalEnvVars(); /*Use the global environment variables we have set.*/

	PrintBootBanner();

	if (EnableLogging)
	{
		WriteLogLine(CONSOLE_COLOR_CYAN VERSIONSTRING " Booting up\n" "Compiled " __DATE__ " " __TIME__ CONSOLE_ENDCOLOR "\n", true);
	}

	MountVirtuals(); /*Mounts any virtual filesystems, upon request.*/

	if (Hostname[0] != '\0')
	{ /*The system hostname.*/
		char TmpBuf[MAX_LINE_SIZE];
		
		if (sethostname(Hostname, strlen(Hostname)) == 0)
		{
			snprintf(TmpBuf, MAX_LINE_SIZE, "Hostname set to \"%s\".", Hostname);
		}
		else
		{
			snprintf(TmpBuf, MAX_LINE_SIZE, "Unable to set hostname to \"%s\"!\n"
					"Ensure that this is a valid hostname.", Hostname);
			SpitWarning(TmpBuf);
		}
		
		WriteLogLine(TmpBuf, true);
	}
	
	if (*Domainname != '\0')
	{ /*The domain name. Not actually used much but still an important feature.*/
		char TmpBuf[MAX_LINE_SIZE];
		
		if (setdomainname(Domainname, strlen(Domainname)) == 0)
		{
			snprintf(TmpBuf, sizeof TmpBuf, "Domain name set to \"%s\".", Domainname);
		}
		else
		{
			snprintf(TmpBuf, sizeof TmpBuf, "Unable to set domain name to \"%s\"!\n"
					"Ensure that this is a properly formatted domain name.", Domainname);
			SpitWarning(TmpBuf);
		}
		
		WriteLogLine(TmpBuf, true);
	}
	
	if (DisableCAD)
	{	
		const char *CADMsg[2] = { "Krystart has taken control of CTRL-ALT-DEL events.",
							"Krystart was unable to take control of CTRL-ALT-DEL events." };
			
		if (!reboot(OSCTL_DISABLE_CTRLALTDEL)) /*Disable instant reboot on CTRL-ALT-DEL.*/
		{			
			WriteLogLine(CADMsg[0], true);
		}
		else
		{
			WriteLogLine(CADMsg[1], true);
			SpitWarning(CADMsg[1]);
		}
	}
	else
	{
		WriteLogLine("Krystart will not request control of CTRL-ALT-DEL events.", true);
	}

	WriteLogLine(CONSOLE_COLOR_YELLOW "Starting all objects.\n" CONSOLE_ENDCOLOR, true);
	
	if (!RunAllObjects(true))
	{
		EmergencyShell();
	}
	
	FinaliseLogStartup(BlankLogOnBoot); /*Write anything in the log's memory to disk.
		* NOTE: It's possible for data to be in here even if logging is disabled, so don't touch.*/
					
	WriteLogLine(CONSOLE_COLOR_GREEN "Bootup complete.\n" CONSOLE_ENDCOLOR, true);

	if (!InitMemBus(true))
	{
		const char *MemBusErr = CONSOLE_COLOR_RED "FAILURE IN MEMBUS! "
								"You won't be able to shut down the system with Krystart!"
								CONSOLE_ENDCOLOR;
		
		SpitError(MemBusErr);
		WriteLogLine(MemBusErr, true);
		
		putc('\007', stderr); /*Beep.*/
	}
	
	PrimaryLoop(); /*Does everything after initial boot.*/
}

void LaunchShutdown(int Signal)
{ /*Responsible for reboot, halt, power down, etc.*/
	char MsgBuf[MAX_LINE_SIZE];
	const char *HType = NULL;
	const char *AttemptMsg = NULL;
	const char *LogMsg = ((Signal == OSCTL_HALT || Signal == OSCTL_POWEROFF) ?
						CONSOLE_COLOR_RED "Shutting down." CONSOLE_ENDCOLOR :
						CONSOLE_COLOR_RED "Rebooting." CONSOLE_ENDCOLOR);
	
	switch (Signal)
	{
		case OSCTL_HALT:
			HType = "halt";
			break;
		case OSCTL_POWEROFF:
			HType = "poweroff";
			break;
		default:
			HType = "reboot";
			break;
	}
	
	snprintf(MsgBuf, sizeof MsgBuf, "System is going down for %s NOW!", HType);
	EmulWall(MsgBuf, false);
	
	if (!BlankLogOnBoot) /*No point in doing it if it's just going to be erased.*/
	{
		WriteLogLine(LogMsg, true);
	}
	

	EnableLogging = false; /*Prevent any additional log entries.*/
	
	/*Kill any running jobs.*/
	if (CurrentTask.Set)
	{
		if (CurrentTask.PID == 0)
		{
			Bool *TNum = (void*)CurrentTask.Node;
			
			*TNum = true;
		}
		else
		{
			kill(CurrentTask.PID, SIGKILL);
			waitpid(CurrentTask.PID, NULL, 0); /*Reap it.*/
		}
		
		CurrentTask.Set = false;
		CurrentTask.Node = NULL;
		CurrentTask.TaskName = NULL;
		CurrentTask.PID = 0;
	}
	
	
	if (!ShutdownMemBus(true))
	{ /*Shutdown membus first, so no other signals will reach us.*/
		SpitWarning("Failed to shut down membus interface.");
	}
	
	if (Signal == OSCTL_HALT || Signal == OSCTL_POWEROFF)
	{
		printf("%s", CONSOLE_COLOR_RED "Shutting down.\n" CONSOLE_ENDCOLOR "\n");
	}
	else
	{
		printf("%s", CONSOLE_COLOR_RED "Rebooting.\n" CONSOLE_ENDCOLOR "\n");
	}
	
	if (!RunAllObjects(false)) /*Run all the service stopping things.*/
	{
		SpitError("Failed to complete shutdown/reboot sequence!");
		EmergencyShell();
	}
	
	ShutdownConfig();
	
	
	if (Signal == OSCTL_HALT)
	{
		AttemptMsg = "Attempting to halt the system...";
	}
	else if (Signal == OSCTL_POWEROFF)
	{
		AttemptMsg = "Attempting to power down the system...";
	}
	else
	{
		AttemptMsg = "Attempting to reboot the system...";
	}
	
	printf("%s%s%s\n", CONSOLE_COLOR_CYAN, AttemptMsg, CONSOLE_ENDCOLOR);
	
	sync(); /*Force sync of disks in case somebody forgot.*/

	reboot(Signal); /*Send the signal.*/

	
	/*Again, not supposed to be here.*/
	
	SpitError("Failed to reboot/halt/power down!");
	EmergencyShell();
}

static void ApplyGlobalEnvVars(void)
{
	struct _EnvVarList *Worker = GlobalEnvVars;
	
	if (!Worker) return;
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		char OutBuf[MAX_LINE_SIZE];
		
		putenv(Worker->EnvVar);
		snprintf(OutBuf, sizeof OutBuf, "Set global environment variable \"%s\"", Worker->EnvVar);
		WriteLogLine(OutBuf, true);
	}
}
