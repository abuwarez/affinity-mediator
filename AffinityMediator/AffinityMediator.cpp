#include "pch.h"
#include <iostream>
#include <vector>
#include <functional>

#include "windows.h"
#include "tchar.h"
#include "winbase.h"
#include "WinDef.h"
#include "processthreadsapi.h"
#include "assert.h"

int gTotalLCPUs = 0;

struct ExecTimes
{
	ULARGE_INTEGER time;
	ULARGE_INTEGER timeStamp;
	DWORD affinity;
};

int CalcAbsCPUUsage(ExecTimes start, ExecTimes stop);
int GetUsedLCPUCount(DWORD affinityMask);

template <class T> struct RunningAvg
{
	using SampleCalc = std::function<int(const T&, const T&)>;

	const int maxSampleCount;

	std::vector<T> samples;
	int crtSampleIdx;

	RunningAvg(int maxSampleCount) : maxSampleCount(maxSampleCount), crtSampleIdx(0) { samples.reserve(maxSampleCount); }

	void clear()
	{
		crtSampleIdx = 0;
		samples.clear();
	}

	void PushSample(T && s)
	{
		if (crtSampleIdx == samples.size())
			samples.push_back(std::forward<T>(s));
		else
			samples[crtSampleIdx] = std::forward<T>(s);

		++crtSampleIdx %= maxSampleCount;
	}

	int GetAvg(SampleCalc calcFunc) const
	{
		if (samples.size() <= 1)
			return 0;

		int sum = 0;

		size_t startIdx = crtSampleIdx == samples.size() ? 0 : crtSampleIdx;// == 0 ? samples.size() - 2 : (crtSampleIdx - 2) % samples.size();
		for (size_t i = 0; i < samples.size() - 1; ++i)
			sum += calcFunc(samples[(startIdx + i) % samples.size()], samples[(startIdx + i + 1) % samples.size()]);

		return sum /= (samples.size() - 1); //avem samples.size() timpi dar samples.size() - 1 intervale
	}
};

struct ProcessDetails
{
	enum State : int {
		WARMUP = 0,
		BLANA,

		COUNT
	};

	const HANDLE hProcess;
	const DWORD processId;
	State state;
	RunningAvg<ExecTimes> avg;
	DWORD affinity;

	ProcessDetails(HANDLE hProcess, DWORD processId, DWORD affinity) : hProcess(hProcess), processId(processId), state(WARMUP), avg(11), affinity(affinity) {}
};

void printBits(DWORD val, int count)
{
	for (int i = count - 1; i >= 0; --i)
	{
		std::cout << ((val & (1 << i)) ? 1 : 0);
	}
}

int TriviAvg(const int & a, const int &b)
{
	return b;
}

struct Policy
{
	using AffinityMasks = DWORD[ProcessDetails::COUNT];

	struct ProcessData
	{
		ProcessData(ProcessDetails & pd, int count) : pd(pd), affUsage(count) {}

		ProcessDetails & pd;
		AffinityMasks afm;
		RunningAvg<int> affUsage;
	};

	ProcessData p1;
	ProcessData p2;

	DWORD sysAffMask;

	int blanaThr;
	int warmupThr;
	const int warmupLCPUCount;

	DWORD createBitRange(int startIdx, int count)
	{
		DWORD res = 0;

		for (int i = 0; i < count; ++i)
		{
			res |= 1 << (startIdx + i);
		}

		return res;
	}

	Policy(ProcessDetails & pd1, ProcessDetails & pd2, DWORD sysAffMask, int samples, int blanaThr, int warmupThr, int warmupLCPUCount) : p1(pd1, samples), p2(pd2, samples), sysAffMask(sysAffMask), blanaThr(blanaThr), warmupThr(warmupThr), warmupLCPUCount(warmupLCPUCount)
	{
		const int blanaLCPUCount = gTotalLCPUs - warmupLCPUCount;
		const int maxBit = gTotalLCPUs - 1;

		//low order bits
		p1.afm[ProcessDetails::WARMUP] = createBitRange(0, warmupLCPUCount);
		p1.afm[ProcessDetails::BLANA] = createBitRange(0, blanaLCPUCount);

		//high order bits
		p2.afm[ProcessDetails::WARMUP] = createBitRange(gTotalLCPUs - warmupLCPUCount, warmupLCPUCount);
		p2.afm[ProcessDetails::BLANA] = createBitRange(gTotalLCPUs - blanaLCPUCount, blanaLCPUCount);

		printBits(p1.afm[0], gTotalLCPUs); std::cout << " ";
		printBits(p1.afm[1], gTotalLCPUs); std::cout << std::endl;
		printBits(p2.afm[0], gTotalLCPUs); std::cout << " ";
		printBits(p2.afm[1], gTotalLCPUs); std::cout << std::endl;

		bool res = SetProcessAffinityMask(p1.pd.hProcess, p1.afm[ProcessDetails::WARMUP]);
		assert(res);

		res = SetProcessAffinityMask(p2.pd.hProcess, p2.afm[ProcessDetails::WARMUP]);
		assert(res);
	}

	void Update()
	{
		p1.affUsage.PushSample(p1.pd.avg.GetAvg(CalcAbsCPUUsage) / GetUsedLCPUCount(p1.pd.affinity));
		p2.affUsage.PushSample(p2.pd.avg.GetAvg(CalcAbsCPUUsage) / GetUsedLCPUCount(p2.pd.affinity));

		if (p2.affUsage.samples.size() < p2.affUsage.samples.capacity() || p1.affUsage.samples.size() < p1.affUsage.samples.capacity())
			return;

		assert(p1.pd.state != ProcessDetails::BLANA || p2.pd.state != ProcessDetails::BLANA);

		ProcessData * ppd1 = &p1;
		ProcessData * ppd2 = &p2;

		if (ppd2->pd.state == ProcessDetails::BLANA)
		{
			std::swap(ppd1, ppd2);
		}

		int avg1 = ppd1->affUsage.GetAvg(TriviAvg);
		int avg2 = ppd2->affUsage.GetAvg(TriviAvg);

		if (ppd1->pd.state == ProcessDetails::BLANA)
		{
			if (avg1 < warmupThr)
			{
				// frana
				ppd1->pd.state = ProcessDetails::WARMUP;
				ppd1->pd.avg.clear();
				ppd1->affUsage.clear();
				ppd2->affUsage.clear();

				bool res = SetProcessAffinityMask(ppd1->pd.hProcess, ppd1->afm[ProcessDetails::WARMUP]);
				assert(res);
			}

			return;
		}
		else
		{
			// amandoua in warmup
			if (avg1 < avg2)
			{
				std::swap(ppd1, ppd2);
				std::swap(avg1, avg2);
			}

			if (avg1 > blanaThr)
			{
				//blana
				ppd1->pd.state = ProcessDetails::BLANA;
				ppd1->pd.avg.clear();
				ppd1->affUsage.clear();
				ppd2->affUsage.clear();

				bool res = SetProcessAffinityMask(ppd1->pd.hProcess, ppd1->afm[ProcessDetails::BLANA]);
				assert(res);
			}
		}

	}
};

int GetTotalLCPUCount(DWORD sysAffinityMask)
{
	int count = sysAffinityMask ? 1 : 0;
	while (sysAffinityMask && (sysAffinityMask = sysAffinityMask >> 1)) ++count;

	return count;
}

int GetUsedLCPUCount(DWORD affinityMask)
{
	int count = 0;
	do
	{
		count += affinityMask & 1;
		affinityMask >>= 1;
	} while (affinityMask);

	return count;
}

std::pair<HANDLE, DWORD> SpawnProcess()
{
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	char c[] = "C:\\Users\\patruore26\\source\\repos\\CPUUser\\Debug\\CPUUser.exe";


	bool res = CreateProcessA(nullptr,   // No module name (use command line)
		c,        // Command line
		nullptr,           // Process handle not inheritable
		nullptr,           // Thread handle not inheritable
		false,          // Set handle inheritance to FALSE
		0,              // No creation flags
		nullptr,           // Use parent's environment block
		nullptr,           // Use parent's starting directory 
		&si,            // Pointer to STARTUPINFO structure
		&pi         // Pointer to PROCESS_INFORMATION structure
	);

	assert(res);

	return std::make_pair(pi.hProcess, pi.dwProcessId);
}

void ReadExecutionTimes(HANDLE hProcess, ExecTimes &execTimes)
{
	FILETIME createTime = { 0, 0 };
	FILETIME exitTime = { 0, 0 };
	FILETIME kernelTime = { 0, 0 };
	FILETIME userTime = { 0, 0 };

	bool res = GetProcessTimes(hProcess, &createTime, &exitTime, &kernelTime, &userTime);
	assert(res);

	DWORD affinityMask = 0, sysAffinityMask = 0;

	res = GetProcessAffinityMask(hProcess, &affinityMask, &sysAffinityMask);
	assert(res);

	FILETIME crtTime;
	GetSystemTimeAsFileTime(&crtTime);

	ULARGE_INTEGER tmp;
	tmp.LowPart = kernelTime.dwLowDateTime;
	tmp.HighPart = kernelTime.dwHighDateTime;

	execTimes.time.LowPart = userTime.dwLowDateTime;
	execTimes.time.HighPart = userTime.dwHighDateTime;
	execTimes.time.QuadPart += tmp.QuadPart;

	execTimes.timeStamp.LowPart = crtTime.dwLowDateTime;
	execTimes.timeStamp.HighPart = crtTime.dwHighDateTime;

	execTimes.affinity = affinityMask;
}

int CalcAbsCPUUsage(ExecTimes start, ExecTimes stop)
{
	assert(start.timeStamp.QuadPart < stop.timeStamp.QuadPart);

	return static_cast<int>(100 * (stop.time.QuadPart - start.time.QuadPart) / (stop.timeStamp.QuadPart - start.timeStamp.QuadPart));
}

void InitConsole()
{
	HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD dwMode;

	GetConsoleMode(hOutput, &dwMode);

	dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	SetConsoleMode(hOutput, dwMode);
}

void PrintProcessDetails(const ProcessDetails & pd)
{
	int avgTime = pd.avg.GetAvg(CalcAbsCPUUsage);
	int affLCPUs = GetUsedLCPUCount(pd.affinity);

	std::cout << pd.processId << " state: " << static_cast<int>(pd.state) << " affinity: ";
	printBits(pd.affinity, gTotalLCPUs);
	std::cout << " sys avg: " << avgTime / gTotalLCPUs << " aff avg: " << avgTime / affLCPUs << "      ";
}

char printspinner(int idx)
{
	const char * states = "|/-\\";
	return states[idx % 4];
}

void PrintPolicyStatus(const Policy & p)
{
	std::cout << "Policy: " << p.p1.pd.processId << " avg: " << p.p1.affUsage.GetAvg(TriviAvg) << "/" << p.p1.affUsage.samples.size() << "; "
		<< p.p2.pd.processId << " avg: " << p.p2.affUsage.GetAvg(TriviAvg) << "/" << p.p2.affUsage.samples.size() << "         ";
}

int main(int argc, char ** argv)
{
	InitConsole();

	int updInt = 500;
	int policySamples = 10;
	int blanaThr = 90;
	int warmupThr = 60;
	int warmupLCPUCount = 1;

	if (argc != 3 && argc != 8)
	{
		std::cout << "Pidurile, vere: " << argv[0] << " <cinema pid 1> <cinema pid 2> [<updInt> <policySamples> <blanaThr> <warmupThr> <warmupLCPUCount>]" << std::endl;
		std::cout << " cinema pid 1&2 - din task manager" << std::endl;
		std::cout << " updInt - interval verificare usage(ms). default = " << updInt << std::endl;
		std::cout << " policySamples - cate samples in media alunecatoare pt sabilirea regimului proceselor. default = " << policySamples << std::endl;
		std::cout << "    in combinatie cu updInt, policySamples stabileste timpul minim de reactie = updInt*policySamples" << std::endl;
		std::cout << "    default = " << updInt << "ms * " << policySamples << " = " << (updInt * policySamples / 1000) << "sec" << std::endl;
		std::cout << " blanaThr - usage mediu al unui process timp de policySamples ca sa treaca in blana, procente. default = " << blanaThr << std::endl;
		std::cout << " warmupThr - usage mediu al unui process timp de policySamples ca sa treaca din blana in warmup, procente, default = " << warmupThr << std::endl << std::endl;
		std::cout << " warmupLCPUCount - nr de cpuuri logice alocat warmup-ului, default = " << warmupLCPUCount << std::endl << std::endl;

		return -1;
	}

	int pid1 = atoi(argv[1]);
	HANDLE hProcess1 = OpenProcess(PROCESS_ALL_ACCESS, false, pid1);
	DWORD p1AffinityMask = 0, sysAffinityMask = 0;

	if (hProcess1 == nullptr)
	{
		std::cout << "err pid 1 (" << pid1 << "): " << std::hex << GetLastError() << std::endl;
		return 1;
	}

	bool res = GetProcessAffinityMask(hProcess1, &p1AffinityMask, &sysAffinityMask);
	if (!res)
	{
		std::cout << "err aff 1 (" << pid1 << "): " << std::hex << GetLastError() << std::endl;
		return 1;
	}

	int pid2 = atoi(argv[2]);
	HANDLE hProcess2 = OpenProcess(PROCESS_ALL_ACCESS, false, pid2);
	DWORD p2AffinityMask = 0;

	if (hProcess2 == nullptr)
	{
		std::cout << "err pid 2 (" << pid2 << "): " << std::hex << GetLastError() << std::endl;
		return 2;
	}

	res = GetProcessAffinityMask(hProcess2, &p2AffinityMask, &sysAffinityMask);
	if (!res)
	{
		std::cout << "err aff 2 (" << pid2 << "): " << std::hex << GetLastError() << std::endl;
		return 1;
	}

	if (argc == 8)
	{
		updInt = atoi(argv[3]);
		policySamples = atoi(argv[4]);
		blanaThr = atoi(argv[5]);
		warmupThr = atoi(argv[6]);
		warmupLCPUCount = atoi(argv[7]);
	}

	std::cout << "updInt = " << updInt << std::endl;
	std::cout << "policySamples = " << policySamples << std::endl;
	std::cout << "blanaThr = " << blanaThr << std::endl;
	std::cout << "warmupThr = " << warmupThr << std::endl;
	std::cout << "warmupLCPUCount = " << warmupLCPUCount << std::endl;

	gTotalLCPUs = GetTotalLCPUCount(sysAffinityMask);
	std::cout << "logical CPUs: " << gTotalLCPUs << std::endl;

	ProcessDetails p1(hProcess1, pid1, p1AffinityMask);
	ProcessDetails p2(hProcess2, pid2, p2AffinityMask);

	Policy p(p1, p2, sysAffinityMask, policySamples, blanaThr, warmupThr, warmupLCPUCount);

	int s = 0;

	while (1)
	{
		ExecTimes execTimes1 = { 0, 0, 0 };
		ReadExecutionTimes(p1.hProcess, execTimes1);

		if (p1.affinity != execTimes1.affinity)
		{
			p1.avg.clear();
			p1.affinity = execTimes1.affinity;
		}

		p1.avg.PushSample(std::move(execTimes1));

		std::cout << printspinner(s / (p1.state == ProcessDetails::WARMUP ? 3 : 1)) << " ";
		PrintProcessDetails(p1);
		std::cout << std::endl;

		ExecTimes execTimes2 = { 0, 0, 0 };
		ReadExecutionTimes(p2.hProcess, execTimes2);

		if (p2.affinity != execTimes2.affinity)
		{
			p2.avg.clear();
			p2.affinity = execTimes2.affinity;
		}

		p2.avg.PushSample(std::move(execTimes2));


		std::cout << printspinner(s++ / (p2.state == ProcessDetails::WARMUP ? 3 : 1)) << " ";
		PrintProcessDetails(p2);

		std::cout << std::endl;
		PrintPolicyStatus(p);

		std::cout << "\r\033[2A";

		p.Update();

		Sleep(updInt);
	}

	return 0;
}
