#include <windows.h>
#include <MMSystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <comdef.h>
#include <stdio.h>
#include "../Logger.h"
#include <FunctionDiscoveryKeys_devpkey.h>
#include <string>


#define PACKETSIZE 16384

int main(int argc, char *argv[]);
HRESULT recordWasapi(int audioSamplePerSec, int audioChannels, int audioBytesPerSample, int audioBufferChunk, int audioBufferNum, std::string name, bool forcemono, bool audioMuLaw);
HRESULT getDefaultDevice(std::string name, IMMDevice **ppMMDevice, bool *isCapture);
void mulaw_encode (int16_t * in, uint8_t * out, int numsamples);

SOCKET connection = INVALID_SOCKET;
SOCKADDR_IN sin_server;

void atexitFunction() {
	char dummy;
	sendto(connection, &dummy, 0, 0, (SOCKADDR*)&sin_server, sizeof(sin_server));
}

BOOL WINAPI consoleHandlerRoutine(DWORD) {
	atexitFunction();
	return FALSE;
}

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	return main(__argc, __argv);
}

int main(int argc, char *argv[])
{
	const char *ip = "192.168.1.13";
	int port = 50000;
	int audioSamplePerSec = 48000;
	int audioChannels = 2;
	int audioBytesPerSample = 2;
	int audioBufferChunk = 240;
	int audioBufferNum = 2;
	bool audioForceMono = false;
	bool audioMuLaw = false;
	std::string audioCaptureName ="Microphone";

	WSADATA wsadata;

	WSAStartup(MAKEWORD(2, 0), &wsadata);

	for(int i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "--rate") && (i+1) < argc) {
			audioSamplePerSec = atoi(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--channel") && (i+1) < argc) {
			audioChannels = atoi(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--chunksize") && (i+1) < argc) {
			audioBufferChunk = atoi(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--chunknum") && (i+1) < argc) {
			audioBufferNum = atoi(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--port") && (i+1) < argc) {
			port = atoi(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--device") && (i+1) < argc) {
			audioCaptureName = std::string(argv[i+1]);
			i++;
		} else if(!strcmp(argv[i], "--forcemono")) {
			audioForceMono = true;
		} else if(!strcmp(argv[i], "--ulaw")) {
			audioMuLaw = true;
		} else {
			ip = argv[i];
		}
	}

	if (audioForceMono) printf("Forcing mono\n");
	if (audioMuLaw) printf("Using uLaw companding\n");
	printf("Connecting to: %s:%d\n", ip, port);
	sin_server.sin_addr.s_addr = inet_addr(ip);
	sin_server.sin_family = AF_INET;
	sin_server.sin_port = htons(port);

	if((connection = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
	{
		MessageBox(NULL, "Error Winsock", "NetSound", MB_OK);
		return -1;
	}

	atexit(atexitFunction);
	SetConsoleCtrlHandler(consoleHandlerRoutine, TRUE);
	
	unsigned long flags = 1;
	ioctlsocket(connection, FIONBIO, &flags);

	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	int errcode = recordWasapi(audioSamplePerSec, audioChannels, audioBytesPerSample, audioBufferChunk, audioBufferNum, audioCaptureName, audioForceMono, audioMuLaw);
	if(errcode) {
		_com_error err(errcode);
		LPCTSTR errMsg = err.ErrorMessage();
		logMessage("Error: %s\n", errMsg);
	}

	closesocket(connection);

	WSACleanup();
	return 0;
}

void sendAudioData(const void* data, unsigned int size) {
	int datasent;
	int result;
	for(datasent = 0 ; datasent < (int)size;) {
		result = sendto(connection, ((char*)data + datasent), min(size - datasent, PACKETSIZE), 0, (SOCKADDR*)&sin_server, sizeof(sin_server));
		if(result <= 0) {
			logMessage("Error sendto: %d, errno: %d\n", result, errno);
		} else {
			datasent += result;
		}
	}
}

// -------------------------------
// WASAPI

#define REFTIMES_PER_SEC  10000000L
#define REFTIMES_PER_MILLISEC  10000L

#define EXIT_ON_ERROR(hres)  \
			  if (FAILED(hres)) { printError(hres, __LINE__); goto Exit; }
#define SAFE_RELEASE(punk)  \
			  if ((punk) != NULL)  \
				{ (punk)->Release(); (punk) = NULL; }

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

HRESULT recordWasapi(int audioSamplePerSec, int audioChannels, int audioBytesPerSample, int audioBufferChunk, int audioBufferNum, std::string name, bool audioForceMono, bool audioMuLaw)
{
	HRESULT hr;
	UINT32 bufferFrameCount;
	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDevice *pDevice = NULL;
	bool isCapture = true;
	IAudioClient *pAudioClient = NULL;
	IAudioCaptureClient *pCaptureClient = NULL;
	WAVEFORMATEX pFormat;
	int timePeriod = audioBufferChunk * 1000 / audioSamplePerSec;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	EXIT_ON_ERROR(hr)

	timeBeginPeriod(timePeriod);

	hr = getDefaultDevice(name, &pDevice, &isCapture);
	EXIT_ON_ERROR(hr)

	if(pDevice == NULL) {
		printf("Device %s not found\n", name.c_str());
		return S_FALSE;
	}

	hr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
	EXIT_ON_ERROR(hr)

	pFormat.wFormatTag = WAVE_FORMAT_PCM;
	pFormat.nChannels = audioChannels;
	pFormat.wBitsPerSample = audioBytesPerSample*8;
	pFormat.nBlockAlign = pFormat.nChannels * audioBytesPerSample;
	pFormat.nSamplesPerSec = audioSamplePerSec;
	pFormat.nAvgBytesPerSec = pFormat.nBlockAlign * pFormat.nSamplesPerSec;
	pFormat.cbSize = 0;

	hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
								  AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED | (!isCapture ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0),
								  timePeriod * REFTIMES_PER_MILLISEC * audioBufferNum,
								  0,
								  &pFormat,
								  NULL);

	EXIT_ON_ERROR(hr)

	// Get the size of the allocated buffer.
	hr = pAudioClient->GetBufferSize(&bufferFrameCount);
	EXIT_ON_ERROR(hr)

	printAudioConfig(pAudioClient);

	timeBeginPeriod(1);

	hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
	EXIT_ON_ERROR(hr)

	logMessage("Playing\n");

	hr = pAudioClient->Start();  // Start recording.
	EXIT_ON_ERROR(hr)

	// Each loop fills about half of the shared buffer.
	while(1) {
		UINT32 packetLength = 0;

		// Sleep for one period.
		Sleep(timePeriod-1);

		hr = pCaptureClient->GetNextPacketSize(&packetLength);
		EXIT_ON_ERROR(hr)

		while(packetLength != 0) {
			UINT32 numFramesAvailable;
			BYTE *pData;
			DWORD flags;

			// Get the available data in the shared buffer.
			hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
			EXIT_ON_ERROR(hr)

			UINT32 bufferBytes = numFramesAvailable * pFormat.nBlockAlign;
			if(audioForceMono) bufferBytes = bufferBytes / audioChannels;
			uint8_t *sendBuffer = new uint8_t[bufferBytes];

			if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
				memset(sendBuffer, 0, bufferBytes);
			else {
				if(audioForceMono){
					uint8_t* sendBufferIn = sendBuffer;
					uint8_t* pDataOut = pData;
					// Copy even numbered samples to out buffer
					for (size_t i = 0; i < numFramesAvailable; i++)
					{
						for (size_t j = 0; j < audioBytesPerSample; j++)
						{
							*(sendBufferIn + j) = *(pDataOut + j);
						}
						sendBufferIn += audioBytesPerSample;
						pDataOut += pFormat.nBlockAlign;
					}
				}
				else {
					memcpy(sendBuffer, pData, bufferBytes);
				}
			}

			if(audioMuLaw && audioBytesPerSample == 2){
				int16_t *inPointer = (int16_t *)sendBuffer;
				uint8_t *outPointer = sendBuffer;
				bufferBytes = bufferBytes/audioBytesPerSample; 
				mulaw_encode(inPointer, outPointer, bufferBytes);
			}

			hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
			EXIT_ON_ERROR(hr)

			if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
				logMessage("Error: discontinuity\n");

			if(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
				logMessage("Error: timestamp error\n");

			sendAudioData(sendBuffer, bufferBytes);
			delete[] sendBuffer;

			hr = pCaptureClient->GetNextPacketSize(&packetLength);
			EXIT_ON_ERROR(hr)
		}
	}

	hr = pAudioClient->Stop();  // Stop recording.
	EXIT_ON_ERROR(hr)

Exit:
	timeEndPeriod(1);
	SAFE_RELEASE(pEnumerator)
	SAFE_RELEASE(pDevice)
	SAFE_RELEASE(pAudioClient)
	SAFE_RELEASE(pCaptureClient)

	return hr;
}

HRESULT getDeviceByName(std::string name, IMMDeviceEnumerator *pMMDeviceEnumerator, IMMDevice **ppMMDevice, EDataFlow dataFlow) {
	HRESULT hr = S_OK;
	IMMDeviceCollection *pMMDeviceCollection = NULL;
	UINT count = 0;

	hr = pMMDeviceEnumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &pMMDeviceCollection);
	EXIT_ON_ERROR(hr)

	hr = pMMDeviceCollection->GetCount(&count);
	EXIT_ON_ERROR(hr)

	for(UINT i = 0; i < count; i++) {
		IMMDevice* pEndpoint;
		IPropertyStore *pProps = NULL;
		PROPVARIANT varName;
		char deviceName[512];

		pMMDeviceCollection->Item(i, &pEndpoint);

		hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
		if (FAILED(hr)) {
			printf("IMMDevice(OpenPropertyStore) failed: hr = 0x%08x\n", hr);
			pEndpoint->Release();
			continue;
		}

		// Initialize container for property value.
		PropVariantInit(&varName);

		// Get the endpoint's friendly-name property.
		hr = pProps->GetValue(PKEY_Device_DeviceDesc, &varName);
		if (FAILED(hr)) {
			printf("IPropertyStore(GetValue) failed: hr = 0x%08x\n", hr);
			pProps->Release();
			pEndpoint->Release();
			continue;
		}

		sprintf(deviceName, "%S", varName.pwszVal);
		PropVariantClear(&varName);
		pProps->Release();

		if(name == deviceName) {
			printf("Using device %s\n", deviceName);
			*ppMMDevice = pEndpoint;
			break;
		} else {
			pEndpoint->Release();
		}
	}

Exit:
	SAFE_RELEASE(pMMDeviceCollection)

	return hr;
}

HRESULT getDefaultDevice(std::string name, IMMDevice **ppMMDevice, bool* isCapture) {
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *pMMDeviceEnumerator;
	// activate a device enumerator
	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&pMMDeviceEnumerator
	);
	EXIT_ON_ERROR(hr)

	*ppMMDevice = NULL;
	hr = getDeviceByName(name, pMMDeviceEnumerator, ppMMDevice, eCapture);
	EXIT_ON_ERROR(hr)

	if(*ppMMDevice != NULL) {
		*isCapture = true;
	} else {
		hr = getDeviceByName(name, pMMDeviceEnumerator, ppMMDevice, eRender);
		EXIT_ON_ERROR(hr)
		*isCapture = false;
	}

	if(*ppMMDevice == NULL) {
		// get the default render endpoint
		EDataFlow dataFlow = name == "mic" ? eCapture : eRender;
		*isCapture = dataFlow == eCapture;
		printf("Using default %s device\n", dataFlow == eCapture ? "capture": "playback");
		hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, ppMMDevice);
		pMMDeviceEnumerator->Release();
		if (FAILED(hr)) {
			printf("IMMDeviceEnumerator::GetDefaultAudioEndpoint failed: hr = 0x%08x\n", hr);
			return hr;
		}
	}

Exit:
	SAFE_RELEASE(pMMDeviceEnumerator)

	return hr;
}

#undef ZEROTRAP                 /* turn on the trap as per the MIL-STD */
#define BIAS 0x84               /* define the add-in bias for 16 bit samples */
#define CLIP 32635

void mulaw_encode (int16_t * in, uint8_t * out, int numsamples)
{
  static int16_t exp_lut[256] = { 0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
  };
  int16_t sign, exponent, mantissa, i;
  int16_t sample;
  uint8_t ulawbyte;

  for (i = 0; i < numsamples; i++) {
    sample = in[i];
      /** get the sample into sign-magnitude **/
    sign = (sample >> 8) & 0x80;        /* set aside the sign */
    if (sign != 0) {
      sample = -sample;         /* get magnitude */
    }
    /* sample can be zero because we can overflow in the inversion,
     * checking against the unsigned version solves this */
    if (((uint16_t) sample) > CLIP)
      sample = CLIP;            /* clip the magnitude */

      /** convert from 16 bit linear to ulaw **/
    sample = sample + BIAS;
    exponent = exp_lut[(sample >> 7) & 0xFF];
    mantissa = (sample >> (exponent + 3)) & 0x0F;
    ulawbyte = ~(sign | (exponent << 4) | mantissa);
#ifdef ZEROTRAP
    if (ulawbyte == 0)
      ulawbyte = 0x02;          /* optional CCITT trap */
#endif
    out[i] = ulawbyte;
  }
}