/*
Audaper.cpp

Speech auditory feedback manipulation program capable of the following types 
of perturbations:
	1) Formants frequencies (F1 and F2)
	2) Pitch
	3) Fine-scale timing (time warping)
	4) Global time delay (DAF)
	5) Local intensity
	6) Global intensity

Heuristics-based online utterance status tracking (OST) is incorporated

Requires ASIO compatible sound cards.

Also incorporated
	1) Cepstral liftering (optional)
	2) Dynamic programming style formant tracking 
		(Xia and Espy-Wilson, 2000, ICSLP)
	3) Gain adaptation after shifting (optional)
	4) RMS-based formant smoothing (to reduce the influence of subglottal 
		coupling on formant estimates) (optional)
	5) Online status tracking (OST) for focal perturbations on multisyllabic, 
		connected speech
	6) Perturbation configuration files (PCFs) for flexible, mixable 
		configuration of perturbation

Authors:
	2007 Marc Boucek, Satrajit Ghosh
	2008-2013 Shanqing Cai (shanqing.cai@gmail.com)

Developed at:
	Speech Communication Group, RLE, MIT
	Speech Laboratory, Boston University
*/

//#include <string.h> 
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <ctype.h>

#define TRACE printf

#include "Audapter.h"
#include <math.h>

//#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <algorithm>
#include "mex.h"

using namespace std;

/* Right shift */
#define RSL(INTEGER,SHIFT) (int)( ( (unsigned)INTEGER ) >> SHIFT )

#ifdef TIME_IT      
LARGE_INTEGER freq, time1, time2;
LONGLONG overhead;
#endif

// Non-class function that is called by the audioIO routines when data becomes available
int audapterCallback(char *buffer, int buffer_size, void * data)	//SC The input is 8-bit, so char type is proper for buffer.
{
	Audapter *audapter = (Audapter*)data;	//SC copy data to audapter
#ifdef TIME_IT
	QueryPerformanceCounter(&time1);
#endif

	if (audapter->actionMode == Audapter::PROC_AUDIO_INPUT_ONLINE)
		audapter->handleBuffer((dtype *)buffer, (dtype *)buffer, buffer_size, false);
	if (audapter->actionMode == Audapter::PROC_AUDIO_INPUT_OFFLINE)
		audapter->handleBuffer((dtype *)buffer, (dtype *)buffer, buffer_size, true);
	else if (audapter->actionMode == Audapter::GEN_SINE_WAVE)
		audapter->handleBufferSineGen((dtype *)buffer, (dtype *)buffer, buffer_size);
	else if (audapter->actionMode == Audapter::GEN_TONE_SEQ)
		audapter->handleBufferToneSeq((dtype*)buffer, (dtype*)buffer, buffer_size);
	else if (audapter->actionMode == Audapter::WAV_PLAYBACK)
		audapter->handleBufferWavePB((dtype*)buffer, (dtype*)buffer, buffer_size);

#ifdef TIME_IT
	QueryPerformanceCounter(&time2);
	TRACE("%.6f\n",double(time2.QuadPart - time1.QuadPart - overhead)/double(freq.QuadPart));
#endif
	return 0;
}

Parameter::paramType Parameter::checkParam(const char *name) {
	string inputNameStr = string(name);
	transform(inputNameStr.begin(), inputNameStr.end(), inputNameStr.begin(), ::tolower);

	int i;
	for (i = 0; i < names.size(); i++) {
		string nameStr = string(names[i]);
		if (nameStr == inputNameStr)
			break;
	}

	if (i >= names.size()) {
		return Parameter::TYPE_NULL;
	}
	else {
		return types[i];
	}

}

/* Constructor of Audapter */
Audapter :: Audapter()
	: downSampFilter(nCoeffsSRFilt), upSampFilter(nCoeffsSRFilt), 
	  preEmpFilter(2), deEmpFilter(2), 
	  shiftF1Filter(3), shiftF2Filter(3), 
	  fmtTracker(0), pVoc(0)
{
	/* Set default action mode */
	actionMode = PROC_AUDIO_INPUT_OFFLINE;

	/* Modifiable parameters */
	/* Boolean parameters */
	params.addParam("bshift",		"Formant perturbation switch", Parameter::TYPE_BOOL);
	params.addParam("btrack",		"Formant tracking switch", Parameter::TYPE_BOOL);
	params.addParam("bdetect",		"Formant tracking period detection switch", Parameter::TYPE_BOOL);
	params.addParam("bweight",		"Switch for intensity-weighted smoothing of formant frequencies", Parameter::TYPE_BOOL);
	params.addParam("bcepslift",	"Switch for cepstral liftering for formant trackng", Parameter::TYPE_BOOL);
	params.addParam("bratioshift",	"Switch for ratio-based formant shifting", Parameter::TYPE_BOOL);
	params.addParam("bmelshift",	"Switch for formant shifting based on the mel frequency scale", Parameter::TYPE_BOOL);
	params.addParam("bgainadapt",	"Formant perturbation gain adaptation switch", Parameter::TYPE_BOOL);
	params.addParam("brmsclip",		"Switch for auto RMS intensity clipping (loudness protection)", Parameter::TYPE_BOOL);	//
	params.addParam("bbypassfmt",	"Switch for bypassing formant tracking (for use in pitch shifting and time warping", Parameter::TYPE_BOOL);
	params.addParam("bpitchshift",	"Pitch shifting switch", Parameter::TYPE_BOOL);
	params.addParam("bdownsampfilt", "Down-sampling filter switch", Parameter::TYPE_BOOL);
	params.addParam("mute",			"Global mute switch", Parameter::TYPE_BOOL_ARRAY);
	params.addParam("bpvocmpnorm",	"Phase vocoder amplitude normalization switch", Parameter::TYPE_BOOL);

	/* Integer parameters */
	params.addParam("srate",		"Sampling rate (Hz), after downsampling", Parameter::TYPE_INT);
	params.addParam("framelen",		"Frame length (samples), after downsampling", Parameter::TYPE_INT);
	params.addParam("ndelay",		"Number of delayed frames before an incoming frame is sent back", Parameter::TYPE_INT);
	params.addParam("nwin",			"Length of an internal frame (frames)", Parameter::TYPE_INT);
	params.addParam("nlpc",			"Order of LPC", Parameter::TYPE_INT);
	params.addParam("nfmts",		"Number of formants to be shifted", Parameter::TYPE_INT);
	params.addParam("ntracks",		"Number of formants to be tracked", Parameter::TYPE_INT);
	params.addParam("avglen",		"Formant smoothing window length (frames)", Parameter::TYPE_INT);
	params.addParam("cepswinwidth", "Window width for cepstral liftering", Parameter::TYPE_INT);
	params.addParam("fb",			"Feedback mode (0-mute, 1-normal, 2-masking noise, 3-speech+noise, 4-speech modulated noise", Parameter::TYPE_INT);
	params.addParam("minvowellen",  "Minimum vowel length (frames)", Parameter::TYPE_INT);
	params.addParam("pvocframelen", "Phase vocoder frame length (samples)", Parameter::TYPE_INT);	//
	params.addParam("pvochop",		"Phase vocoder frame hop (samples)", Parameter::TYPE_INT);	//
	params.addParam("nfb",			"Number of feedbac voices", Parameter::TYPE_INT);
	params.addParam("tsgntones",	"Tone sequence generator: number of tones", Parameter::TYPE_INT);
	params.addParam("downfact",		"Downsampling factor", Parameter::TYPE_INT);
	params.addParam("stereomode",	"Two-channel mode", Parameter::TYPE_INT);

	/* Integer array parameters */
	params.addParam("pvocampnormtrans", "Phase vocoder amplitude normalization transitional period length (frames)", Parameter::TYPE_INT_ARRAY);
	params.addParam("delayframes",	"DAF global delay (frames): maxNVoices-long array", Parameter::TYPE_INT_ARRAY);

	/* Double parameters */
	params.addParam("scale",		"Output scaling factor (gain)", Parameter::TYPE_DOUBLE);
	params.addParam("preemp",		"Pre-emphasis factor", Parameter::TYPE_DOUBLE);
	params.addParam("rmsthr",		"RMS intensity threshold", Parameter::TYPE_DOUBLE);
	params.addParam("rmsratio",		"RMS ratio threshold", Parameter::TYPE_DOUBLE);
	params.addParam("rmsff",		"Forgetting factor for RMS intensity smoothing", Parameter::TYPE_DOUBLE);
	params.addParam("dfmtsff",		"Forgetting factor for formant smoothing (in status tracking)", Parameter::TYPE_DOUBLE);	//
	params.addParam("rmsclipthresh", "Auto RMS intensity clipping threshold (loudness protection)", Parameter::TYPE_DOUBLE);	//

	params.addParam("wgfreq",		"Waveform generator: sine-wave frequency (Hz)", Parameter::TYPE_DOUBLE);
	params.addParam("wgamp",		"Waveform generator: sine-wave peak amplitude", Parameter::TYPE_DOUBLE);
	params.addParam("wgtime",		"Waveform generator: sine-wave duration (s)", Parameter::TYPE_DOUBLE);

	params.addParam("f2min",		"Formant perturbation field: minimum F2 (Hz)", Parameter::TYPE_DOUBLE);
	params.addParam("f2max",		"Formant perturbation field: maximum F2 (Hz)", Parameter::TYPE_DOUBLE);
	params.addParam("f1min",		"Formant perturbation field: minimum F1 (Hz)", Parameter::TYPE_DOUBLE);
	params.addParam("f1max",		"Formant perturbation field: maximum F1 (Hz)", Parameter::TYPE_DOUBLE);
	params.addParam("lbk",			"Formant perturbation field: Oblique lower border: Slope k", Parameter::TYPE_DOUBLE);
	params.addParam("lbb",			"Formant perturbation field: Oblique lower border: Intercept b", Parameter::TYPE_DOUBLE);

	params.addParam("triallen",		"Trial length (s)", Parameter::TYPE_DOUBLE);
	params.addParam("ramplen",		"Audio ramp length (s)", Parameter::TYPE_DOUBLE);

	params.addParam("afact",		"Formant-tracking algorithm: alpha", Parameter::TYPE_DOUBLE);
	params.addParam("bfact",		"Formant-tracking algorithm: beta", Parameter::TYPE_DOUBLE);
	params.addParam("gfact",		"Formant-tracking algorithm: gamma", Parameter::TYPE_DOUBLE);
	params.addParam("fn1",			"Formant-tracking algorithm: F1 prior", Parameter::TYPE_DOUBLE);
	params.addParam("fn2",			"Formant-tracking algorithm: F2 prior", Parameter::TYPE_DOUBLE);

	params.addParam("pitchshiftratio", "Pitch-shifting: ratio (1.0 = no shift)", Parameter::TYPE_DOUBLE_ARRAY);

	params.addParam("rmsff_fb",		"Speech-modulated noise feedback: RMS forgetting factor", Parameter::TYPE_SMN_RMS_FF);
	params.addParam("fb4gaindb",	"Speech-modulated noise feedback: intensity gain factor", Parameter::TYPE_DOUBLE);
	params.addParam("fb3gain",		"Noise gain factor for speech+noise feedback mode", Parameter::TYPE_DOUBLE);
	
	/* Double array parameters */
	params.addParam("datapb",		"Waveform for playback", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("pertf2",		"Formant perturbation field: F2 grid (Hz)", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("pertamp",		"Formant perturbation field: Perturbation vector amplitude", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("pertphi",		"Formant perturbation field: Perturbation vector angle", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("gain",			"Global intensity gain", Parameter::TYPE_DOUBLE_ARRAY);

	params.addParam("tsgtonedur",	"Tone sequence generator: tone durations (s)", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("tsgtonefreq",	"Tone sequence generator: tone frequencies (Hz)", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("tsgtoneamp",	"Tone sequence generator: tone peak amplitudes", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("tsgtoneramp",	"Tone sequence generator: tone ramp durations (s)", Parameter::TYPE_DOUBLE_ARRAY);
	params.addParam("tsgint",		"Tone sequence generator: intervals between tone onsets (s)", Parameter::TYPE_DOUBLE_ARRAY);

	/* Other types of parameters */
	params.addParam("pvocwarp",		"Phase vocoder time warping configuration", Parameter::TYPE_PVOC_WARP);	//

	int		n;
		
	strcpy_s(deviceName, sizeof(deviceName), "MOTU MicroBook");
		
	p.downFact = downSampFact_default;
		
	p.sr				= 96000 / p.downFact;				// internal samplerate (souncard samplerate = p.sr*DOWNSAMP_FACT)
	p.nLPC				= 15;					// LPC order ... number of lpc coeefs= nLPC +1
	p.nFmts				= 2;					// originally the number of formants you want shift ( hardseted = 2)

	// framing and processing 
	p.frameLen			= 192 / p.downFact;		// length of one internal frame ( framelen = nWin * frameshift) (souncard frame length = p.frameLen *DOWNDAMP_FACT)
	p.nDelay			= 7;					// number of delayed framas (of size framelen) before an incoming frame is sent back
												// thus the overall process latency (without souncard) is :Tproc=nDelay*frameLen/sr
	p.bufLen			= (2*p.nDelay-1)*p.frameLen;	// main buffer length : buflen stores (2*nDelay -1)*frameLen samples
		
	p.nWin				= 1;					// number of processes per frame (of frameLen samples)	
	p.frameShift		= p.frameLen/p.nWin;	// number of samples shift between two processes ( = size of processed samples in 1 process)	
	p.anaLen			= p.frameShift+2*(p.nDelay-1)*p.frameLen;// size of lpc analysis (symmetric around window to be processed)
	p.pvocFrameLen			= p.frameShift + (2 * p.nDelay - 3) * p.frameLen;// For frequency/pitch shifting: size of lpc analysis (symmetric around window to be processed)
	p.avgLen			= 10;				    // length of smoothing ( should be approx one pitch period, 
	// can be greater /shorter if you want more / lesss smoothing)
	// avgLen = 1 ---> no smoothing ( i.e. smoothing over one value)

	// RMS
	p.dRMSThresh		= 0.02;	// RMS threshhold for voicing detection
	p.dRMSRatioThresh	= 1.3;	// preemp / original RMS ratio threshhold, for fricative detection 
	p.rmsFF				= 0.9;  // rms forgetting factor for long time rms 

	p.rmsFF_fb[0]		= 0.85; // rms forgetting factor for feedback
	p.rmsFF_fb[1]		= 0.85;	
	p.rmsFF_fb[2]		= 0.0;	// Unit: s: 0.0 means no transition: use only rmsFF_fb[0]
	p.rmsFF_fb[3]		= 0.0;

	rmsFF_fb_now = p.rmsFF_fb[0];

	p.fb4GainDB			= 0.0;	// Gain (in dB) of the feedback-mode-4 speech-modulated noise
	p.fb4Gain			= pow(10.0, p.fb4GainDB / 20);

	p.fb3Gain			= 0.0;

	p.dPreemp			= .98;	// preemphasis factor
	p.dScale			= 1.0;	// scaling the output (when upsampling) (does not affect internal signal

	// for transition detection
	p.dFmtsFF			= 0;	// formant forgeeting factor for s
	p.maxDelta			= 40;	// maximal allowed formant derivative 		
	p.fmtDetectStart[0] = 800;	// formant frequencies at start of transition (goal region),i.e. [a]
	p.fmtDetectStart[1] = 1600;	// formant frequencies at start of transition (goal region),i.e. [a]		
	p.minDetected		= 10;	// min transition detection in row before starting transition		

	// for formant tracking algorithm
	p.trackIntroTime	= 100;	// time in ms
				
	p.aFact			= 1;	// end value ....		
	p.bFact			= 0.8;	// end value ....
	p.gFact			= 1;	// end value ....

	p.fn1			= 633;	// A priori expectation of F1 (Hz)
	p.fn2			= 1333; // A priori expectation of F2 (Hz)

	p.nTracks			= 4;	// number of tracked formants 

	// booleans						
	p.bRecord			= 1;	// record signal, should almost always be set to 1. 
	p.bTrack			= 1;	// use formant tracking algorithm
	p.bShift			= 0;	// do formant shifting	//SC-Mod(09/23/2007): changed from 0 to 1
	p.bGainAdapt		= 1;	// use gain adaption
	p.bDetect			= 0;	// detect transition
	p.bRelative			= 1;	// shift relative to actual formant point, (otherwise absolute coordinate)			
	p.bWeight			= 1;	// do weighted moving average formant smoothing (over avglen) , otherwise not weigthed (= simple moving average)				
	p.bCepsLift			= 0;	//SC-Mod(2008/05/15) Do cepstral lifting by default
		
	p.bRatioShift		= 0;	//SC(2009/01/20)
	p.bMelShift			= 1;	//SC(2009/01/20)

	//SC(2012/03/05)
	p.bPitchShift		= 0;
	for (n = 0; n < maxNVoices; n++)
		p.pitchShiftRatio[n]   = 1.0;	// 1.0 = no shift.
		
	//SC Initialize the playback data and the counter
	for(n=0;n<maxPBSize;n++){
		data_pb[n]      = 0;
	}
	pbCounter			= 0;

	for (n = 0; n < maxToneSeqRecLen; n++) {
		tsg_wf[n] = 0.0;
	}
	tsgRecCounter = 0;

	p.wgFreq			= 1000.;
	p.wgAmp				= 0.1;
	p.wgTime			= 0.;
		
	p.F2Min		= 0;		// Lower boundary of the perturbation field (mel)
	p.F2Max		= 0;		// Upper boundary of the perturbation field (mel)
	p.F1Min		= 0;		// Left boundary of the perturbation field (mel)
	p.F1Max		= 0;		// Right boundary of the perturbation field (mel)
	p.LBk		= 0;		// The slope of a tilted boundary: F2 = p.LBk * F1 + p.LBb. (mel/mel)
	p.LBb		= 0;		// The intercept of a tilted boundary (mel)

	for(n=0;n<pfNPoints;n++){
		p.pertF2[n]=0;			// Independent variable of the perturbation vectors
		p.pertAmp[n]=0;			// Magnitude of the perturbation vectors (mel)
		p.pertPhi[n]=0;			// Angle of the perturbation vectors (rad). 0 corresponds to the x+ axis. Increase in the countetclockwise direction. 
	}

	p.minVowelLen=60;
	p.transDone=false;
	p.transCounter=0;

	//SC(2008/05/15)
	p.cepsWinWidth=30;

	//SC(2008/06/20)
	p.fb=1;		

	//SC(2008/06/22)
	//p.trialLen = 9;	//sec
	//p.rampLen=0.05;	//sec
	p.trialLen = 0.0;	//sec // Zero corresponds to no trial length limit. Data will be stored circularly. 
	p.rampLen = 0.0;	//sec // Zero corresponds to no ramp (sudden onset and offset)

	//SC(2012/02/28) DAF
	for (n = 0; n < maxNVoices; n++) {
		p.delayFrames[n] = 0; // Unit: # of frames (no delay by default)
		p.gain[n] = 1.0;
		p.mute[n] = 0;
	}

	//SC(2012/02/29) For data saving
	dataFileCnt = 0;

	//SC(2012/03/09) Phase vocoder (pvoc) pitch shifting
	p.pvocFrameLen = 1024;
	p.pvocHop = 256;		// 256 = 16 * 16

	p.bDownSampFilt = 1;

	//SC(2012/09/08) BlueShift
	p.nFB = 1;

	//SC(2009/12/01)
	p.tsgNTones=0;
	for (n=0;n<maxNTones;n++){
		p.tsgToneFreq[n]=0;
		p.tsgToneDur[n]=0;
		p.tsgToneAmp[n]=0;
		p.tsgToneRamp[n]=0;
		p.tsgInt[n]=0;
	}

	/* SC (2013-08-06) */
	p.stereoMode = 1;		/* Default: left-right identical */

	/* SC (2013-08-20) */
	p.bPvocAmpNorm = 0;
	p.pvocAmpNormTrans = 16;

	rmsSlopeWin = 0.03; // Unit: s
	rmsSlopeN = (int)(rmsSlopeWin / ((dtype)p.frameLen / (dtype)p.sr));

	intShiftRatio = 1.0;
	amp_ratio = 1.0;
	amp_ratio_prev = 1.0;

	/* Initialize formant tracker */
	try {
		fmtTracker = new LPFormantTracker(p.nLPC, p.sr, p.anaLen, nFFT, p.cepsWinWidth * p.bCepsLift, 
										  p.nTracks, p.aFact, p.bFact, p.gFact, p.fn1, p.fn2, 
										  static_cast<bool>(p.bWeight), p.avgLen);
	}
	catch (LPFormantTracker::initializationError) {
		mexErrMsgTxt("Failed to initialize formant tracker");
	}
	catch (LPFormantTracker::nLPCTooLargeError) {
		mexErrMsgTxt("Failed to initialize formant tracker due to a too larger value in nLPC");
	}

	/* Initialize phase vocoder */
	pVoc = new PhaseVocoder[p.nFB];
		
	for (int i = 0; i < p.nFB; ++i) {
		try {
			pVoc[i].config(PhaseVocoder::operMode::PITCH_SHIFT_ONLY, p.nDelay, 
						   static_cast<dtype>(p.sr), p.frameLen, 
							   p.pvocFrameLen, p.pvocHop);
		}
		catch (PhaseVocoder::initializationError) {
			mexErrMsgTxt("Failed to initialize phase vocoder");
		}

	}

//************************************** Initialize filter coefs **************************************	

	// preemphasis filter
	const dtype t_preemp_a[2] = {1.0, 0.0};
	const dtype t_preemp_b[2] = {1, -p.dPreemp};
	preEmpFilter.setCoeff(2, t_preemp_a, 2, t_preemp_b);

	// deemphasis filter
	const dtype t_deemp_a[2] = {1, -p.dPreemp};
	const dtype t_deemp_b[2] = {1.0, 0.0};
	deEmpFilter.setCoeff(2, t_deemp_a, 2, t_deemp_b);

    // Formant-shifting filters
	a_filt1[0] = 1;
	a_filt2[0] = 1;

	b_filt1[0] = 1;
	b_filt2[0] = 1;

	/* Filters */
	const dtype t_srfilt_a[nCoeffsSRFilt] = {1};
	const dtype t_srfilt_b[nCoeffsSRFilt] = {1};

	downSampFilter.setCoeff(nCoeffsSRFilt, t_srfilt_a, nCoeffsSRFilt, t_srfilt_b);
	upSampFilter.setCoeff(nCoeffsSRFilt, t_srfilt_a, nCoeffsSRFilt, t_srfilt_b);

	//SC(2009/02/06) RMS level clipping protection. 
	p.bRMSClip = 1;
	p.rmsClipThresh = 1.0;

	// For wav file writing
	sprintf_s(wavFileBase, "");

	p.bBypassFmt = 0;

	reset();
}

Audapter::~Audapter(){
	if (fmtTracker) 
		delete fmtTracker;

	if (pVoc)
		delete [] pVoc;
}


void Audapter::reset()
{// resets all 
	int i0,j0;
	bTransReset				= true;

	bFrameLenShown			= false;

//*****************************************************  BUFFERS   *****************************************************
	// Initialize input, output and filter buffers (at original sample rate!!!)
	for(i0 = 0; i0 < maxFrameLen * downSampFact_default; i0++)
	{
		inFrameBuf[i0] = 0.0;

		downSampBuffer[i0] = 0.0;
		upSampBuffer[i0] = 0.0;
	}

	downSampFilter.reset();
	upSampFilter.reset();

	for (i0 = 0; i0 < internalBufLen; i0 ++){
		outFrameBuf[i0] = 0;

		for (j0 = 0; j0 < maxNVoices; j0++)
			outFrameBufPS[j0][i0] = 0;
	}
	outFrameBuf_circPtr = 0;	//Marked

	for (i0 = 0; i0 < maxFrameLen * downSampFact_default; i0++) {
		outFrameBufSum[i0] = 0;
		outFrameBufSum2[i0] = 0;
	}

	// Initialize internal input, output buffers  (at downsampled  rate !!!)
	/*for(i0=0;i0<maxBufLen;i0++)*/
	for(i0 = 0; i0 < maxFrameLen; i0++)
	{
		inBuf[i0] = 0.0;
		outBuf[i0] = 0.0;
		zeros[i0]  = 0.0;
	}

//*****************************************************  FILTER STATES  *****************************************************

	// reinitialize formant shift filter states

	// reinitialize preempahsis and deemphasis filter states
	preEmpFilter.reset();
	deEmpFilter.reset();

	shiftF1Filter.reset();
	shiftF2Filter.reset();

//*****************************************************  RECORDING  *****************************************************

	// Initialize signal recorder
	for(i0=0;i0<2;i0++)
	{
		for(j0=0;j0<maxRecSize;j0++)
		{
			signal_recorder[i0][j0]=0;
		}
	}


	// Initialize data recorder
	for(i0=0;i0<maxDataVec;i0++)
	{
		for(j0=0;j0<maxDataSize;j0++)
		{
			data_recorder[i0][j0]=0;
		}
	}

	for (j0 = 0; j0 < maxDataSize; j0++) {
		a_rms_o[j0] = 0;
		a_rms_o_slp[j0] = 0;
	}

	// Initialize variables used in the pitch shifting algorithm 

	// initialize recording counters
	frame_counter=0;
	frame_counter_nowarp = 0;
	data_counter=0;
	circ_counter=0;

// %%%%%%%%%%%%%%%%%%%%%%%%%%              INITIALYZE FUNCTIONS VARIABLES         %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	

//*****************************************************  getDfmt   *****************************************************


	for(i0=0;i0<2;i0++)
	{
		deltaFmt[i0]=0;
		deltaMaFmt[i0]=0;
		lastFmt[i0]=0;
	}


//*****************************************************  calcRms    *****************************************************	
		
	//initialize  moving average
	ma_rms1 = 0;
	ma_rms2 = 0;	
	ma_rms_fb = 0;

//*****************************************************  getWma    *****************************************************
	//SC(2009/02/02)
	bLastFrameAboveRMS=0;

//*****************************************************  getAI    *****************************************************


//*****************************************************  hqr_Roots    %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	
	p.transDone=false;
	p.transCounter=0;

	//SC(2012/02/29) For data saving (Writing to disk)
	dataFileCnt = 0;
	
	//SC(2012/10/19): OST online sentence tracking
	stat = 0;
	
	duringTimeWarp = false;
	duringTimeWarp_prev = false;

	phase0 = phase1 = 0.0;

	/* Feedback mode 4 */
	rmsFF_fb_now = p.rmsFF_fb[0];
	fb4_status = 0;
	fb4_counter = 0;

	duringPitchShift = false;
	duringPitchShift_prev = false;

	amp_ratio = 1.0;

	/* Reset formant tracker */
	if (fmtTracker)
		fmtTracker->reset();

	/* Reset phase vocoder */
	if (pVoc)
		for (int i = 0; i < p.nFB; ++i)
			pVoc[i].reset();
}

void *Audapter::setGetParam(bool bSet, const char *name, void * value, int nPars, bool bVerbose, int *length) {
	Parameter::paramType pType = params.checkParam(name);
	if (pType == Parameter::TYPE_NULL) {
		string errStr("Unknown parameter name: ");
		errStr += string(name);
		mexErrMsgTxt(errStr.c_str());
	}

	string ns = string(name);
	transform(ns.begin(), ns.end(), ns.begin(), ::tolower);

	void *ptr;
	int len = 1;

	bool bRemakeFmtTracker = false; /* Flag for reinitialization of formant tracker */
	bool bRemakePVoc = false; /* Flag for reinitialization of the phase vocoder */
	/* TODO: code for pvoc reinitialization */

	if (ns == string("bgainadapt")) {
		ptr = (void *)&p.bGainAdapt;
	}
	else if (ns == string("bshift")) {
		ptr = (void *)&p.bShift;
	}
	else if (ns == string("btrack")) {
		ptr = (void *)&p.bTrack;
	}
	else if (ns == string("bdetect")) {
		ptr = (void *)&p.bDetect;
	}
	else if (ns == string("bweight")) {
		ptr = (void *)&p.bWeight;
	}
	else if (ns == string("bcepslift")) {
		ptr = (void *)&p.bCepsLift;

		if (bSet)
			bRemakeFmtTracker = (int)(*((dtype *)value)) != p.bCepsLift;
	}
	else if (ns == string("bratioshift")) {
		ptr = (void *)&p.bRatioShift;
	}
	else if (ns == string("bmelshift")) {
		ptr = (void *)&p.bMelShift;
	}
	else if (ns == string("brmsclip")) {
		ptr = (void *)&p.bRMSClip;
	}
	else if (ns == string("bbypassfmt")) {
		ptr = (void *)&p.bBypassFmt;
	}
	else if (ns == string("srate")) {
		ptr = (void *)&p.sr;

		if (bSet) {
			bRemakeFmtTracker = (int)(*((dtype *)value)) != p.sr;
			bRemakePVoc = (int)(*((dtype *)value)) != p.sr;
		}
	}
	else if (ns == string("framelen")) {
		ptr = (void *)&p.frameLen;

		if (bSet)
			bRemakePVoc = (int)(*((dtype *)value)) != p.frameLen;
	}
	else if (ns == string("ndelay")) {
		ptr = (void *)&p.nDelay;
	}
	else if (ns == string("nwin")) {
		ptr = (void *)&p.nWin;
	}
	else if (ns == string("nlpc")) {
		ptr = (void *)&p.nLPC;

		if (bSet)
			bRemakeFmtTracker = (int)(*((dtype *)value)) != p.nLPC;
	}
	else if (ns == string("nfmts")) {
		ptr = (void *)&p.nFmts;
	}
	else if (ns == string("ntracks")) {
		ptr = (void *)&p.nTracks;

		if (bSet)
			bRemakeFmtTracker = (int)(*((dtype *)value)) != p.nTracks;
	}
	else if (ns == string("avglen")) {
		ptr = (void *)&p.avgLen;
	}
	else if (ns == string("cepswinwidth")) {
		ptr = (void *)&p.cepsWinWidth;

		if (bSet)
			bRemakeFmtTracker = (int)(*((dtype *)value)) != p.cepsWinWidth;
	}
	else if (ns == string("fb")) {
		ptr = (void *)&p.fb;
	}
	else if (ns == string("minvowellen")) {
		ptr = (void *)&p.minVowelLen;
	}
	else if (ns == string("delayframes")) {
		ptr = (void *)p.delayFrames;
		
		if ( bSet && (nPars != p.nFB) )
			mexErrMsgTxt("Erroneous length of input delayFrames");

		len = p.nFB;
	}
	else if (ns == string("bpitchshift")) {
		ptr = (void *)&p.bPitchShift;
	}
	else if (ns == string("pvocframelen")) {
		ptr = (void *)&p.pvocFrameLen;

		if (bSet)
			bRemakePVoc = (int)(*((dtype *)value)) != p.pvocFrameLen;
	}
	else if (ns == string("pvochop")) {
		ptr = (void *)&p.pvocHop;

		if (bSet)
			bRemakePVoc = (int)(*((dtype *)value)) != p.pvocHop;
	}
	else if (ns == string("bdownsampfilt")) {
		ptr = (void *)&p.bDownSampFilt;
	}
	else if (ns == string("nfb")) {
		ptr = (void *)&p.nFB;

		if (bSet)
			bRemakePVoc = (int)(*((dtype *)value)) != p.nFB;
	}
	else if (ns == string("mute")) {
		ptr = (void *)p.mute;		

		if ( bSet && (nPars != p.nFB) )
			mexErrMsgTxt("Erroneous length of input delayFrames");
		
	}
	else if (ns == string("tsgntones")) {
		ptr = (void *)&p.tsgNTones;

		if ( *((int *)ptr) < 0 )
			mexErrMsgTxt("Negative value for tsgNTones");
		if ( *((int *)ptr) > maxNTones ) {
			ostringstream oss;
			oss << "Number of tones in the tone sequence is too big (max allowed = " 
				<< maxNTones << ")";
			mexErrMsgTxt(oss.str().c_str());
		}

	}
	else if (ns == string("downfact")) {
		ptr = (void *)&p.downFact;
	}
	else if (ns == string("stereomode")) {
		ptr = (void *)&p.stereoMode;
	}
	else if (ns == string("bpvocampnorm")) {
		ptr = (void *)&p.bPvocAmpNorm;
	}
	else if (ns == string("pvocampnormtrans")) {
		ptr = (void *)&p.pvocAmpNormTrans;
	}
	else if (ns == string("scale")) {
		ptr = (void *)&p.dScale;
	}
	else if (ns == string("preemp")) {
		ptr = (void *)&p.dPreemp;
	}
	else if (ns == string("rmsthr")) {
		ptr = (void *)&p.dRMSThresh;
	}
	else if (ns == string("rmsratio")) {
		ptr = (void *)&p.dRMSRatioThresh;
	}
	else if (ns == string("rmsff")) {
		ptr = (void *)&p.rmsFF;

		if (bSet) {
			dtype new_val = *((dtype *)value);
			if ( (new_val < 0.0) || (new_val > 1.0) ) 
				mexErrMsgTxt("Invalid input value of rmsFF");
		}
	}
	else if (ns == string("dfmtsff")) {
		ptr = (void *)&p.dFmtsFF;
	}
	else if (ns == string("wgfreq")) {
		ptr = (void *)&p.wgFreq;
	}
	else if (ns == string("wgamp")) {
		ptr = (void *)&p.wgAmp;
	}
	else if (ns == string("wgtime")) {
		ptr = (void *)&p.wgTime;
	}
	else if (ns == string("datapb")) {
		ptr = (void *)data_pb;
		
		if ( bSet && (nPars > maxPBSize) )
			mexErrMsgTxt("Input waveform is too long");

		len = (nPars < maxPBSize) ? nPars : maxPBSize;
	}
	else if (ns == string("f2min")) {
		ptr = (void *)&p.F2Min;
	}
	else if (ns == string("f2max")) {
		ptr = (void *)&p.F2Max;
	}
	else if (ns == string("pertf2")) {
		ptr = (void *)p.pertF2;
		len = pfNPoints;
	}
	else if (ns == string("pertamp")) {
		ptr = (void *)p.pertAmp;
		len = pfNPoints;
	}
	else if (ns == string("pertphi")) {
		ptr = (void *)p.pertPhi;
		len = pfNPoints;
	}
	else if (ns == string("f1min")) {
		ptr = (void *)&p.F1Min;
	}
	else if (ns == string("f1max")) {
		ptr = (void *)&p.F1Max;
	}
	else if (ns == string("lbk")) {
		ptr = (void *)&p.LBk;
	}
	else if (ns == string("lbb")) {
		ptr = (void *)&p.LBb;
	}
	else if (ns == string("triallen")) {
		ptr = (void *)&p.trialLen;

		if (bSet) {
			dtype new_val = *((dtype *)value);
			if ( new_val < 0.0 ) 
				mexErrMsgTxt("Invalid input: negative trial length");
		}
	}
	else if (ns == string("ramplen")) {
		ptr = (void *)&p.rampLen;

		if (bSet) {
			dtype new_val = *((dtype *)value);
			if ( new_val < 0.0 ) 
				mexErrMsgTxt("Invalid input: negative ramp length");
		}
	}
	else if (ns == string("afact")) {
		ptr = (void *)&p.aFact;

		if (bSet)
			bRemakeFmtTracker = (*((dtype *)value)) != p.aFact;
	}
	else if (ns == string("bfact")) {
		ptr = (void *)&p.bFact;

		if (bSet)
			bRemakeFmtTracker = (*((dtype *)value)) != p.bFact;
	}
	else if (ns == string("gfact")) {
		ptr = (void *)&p.gFact;

		if (bSet)
			bRemakeFmtTracker = (*((dtype *)value)) != p.gFact;
	}
	else if (ns == string("fn1")) {
		ptr = (void *)&p.fn1;

		if (bSet)
			bRemakeFmtTracker = (*((dtype *)value)) != p.fn1;
	}
	else if (ns == string("fn2")) {
		ptr = (void *)&p.fn2;

		if (bSet)
			bRemakeFmtTracker = (*((dtype *)value)) != p.fn2;
	}
	else if (ns == string("pitchshiftratio")) {
		ptr = (void *)p.pitchShiftRatio;

		if ( bSet && (nPars != p.nFB) )
			mexErrMsgTxt("Erroneous length of input delayFrames");		
		len = p.nFB;
	}
	else if (ns == string("pvocwarp")) {
		/*ptr = (void *) pertCfg.warpCfg[0];
		if (pertCfg.warpCfg.size() > 0) {
			std::list<pvocWarpAtom>::iterator w_it = pertCfg.warpCfg.begin();

			ptr = (void *) (w_it);
		}*/
		ptr = NULL; //TODO
	}
	else if (ns == string("gain")) {
		ptr = (void *)p.gain;	

		if ( bSet && (nPars != p.nFB) )
			mexErrMsgTxt("Erroneous length of input delayFrames");		
		len = p.nFB;
	}
	else if (ns == string("rmsclipthresh")) {
		ptr = (void *)&p.rmsClipThresh;
	}
	else if (ns == string("tsgtonedur")) {
		ptr = (void *)p.tsgToneDur;
		len = p.tsgNTones;

		if ( bSet && (nPars != len) ) {
			ostringstream oss;
			oss << "Erroneous number of elements in input parameter value: ";
			oss << nPars << " (expected: " << len << ")";			
			mexErrMsgTxt(oss.str().c_str());
		}
	}
	else if (ns == string("tsgtonefreq")) {
		ptr = (void *)p.tsgToneFreq;
		len = p.tsgNTones;

		if ( bSet && (nPars != len) ) {
			ostringstream oss;
			oss << "Erroneous number of elements in input parameter value: ";
			oss << nPars << " (expected: " << len << ")";			
			mexErrMsgTxt(oss.str().c_str());
		}
	}
	else if (ns == string("tsgtoneamp")) {
		ptr = (void *)p.tsgToneAmp;
		len = p.tsgNTones;

		if ( bSet && (nPars != len) ) {
			ostringstream oss;
			oss << "Erroneous number of elements in input parameter value: ";
			oss << nPars << " (expected: " << len << ")";			
			mexErrMsgTxt(oss.str().c_str());
		}
	}
	else if (ns == string("tsgtoneramp")) {
		ptr = (void *)p.tsgToneRamp;
		len = p.tsgNTones;

		if ( bSet && (nPars != len) ) {
			ostringstream oss;
			oss << "Erroneous number of elements in input parameter value: ";
			oss << nPars << " (expected: " << len << ")";
			mexErrMsgTxt(oss.str().c_str());
		}
	}
	else if (ns == string("tsgint")) {
		ptr = (void *)p.tsgInt;
		len = p.tsgNTones;

		if ( bSet && (nPars != len) ) {
			ostringstream oss;
			oss << "Erroneous number of elements in input parameter value: ";
			oss << nPars << " (expected: " << len << ")";			
			mexErrMsgTxt(oss.str().c_str());
		}
	}
	else if (ns == string("rmsff_fb")) {
		ptr = (void *)p.rmsFF_fb;
	}
	else if (ns == string("fb4gaindb")) {
		ptr = (void *)&p.fb4GainDB;
	}
	else if (ns == string("fb3gain")) {
		ptr = (void *)&p.fb3Gain;
	}
	else {		
		string errStr("Unknown parameter name: ");
		errStr += string(name);
		mexErrMsgTxt(errStr.c_str());
		return NULL;
	}

	if (!bSet) {
		/* Get value */
		if (length)
			*length = len;
		return ptr;
	}
	else {
		/* Set value(s) */
		if (pType == Parameter::TYPE_BOOL) {
			*((int *)ptr) = (int)(*((dtype *)value));
		}
		else if (pType == Parameter::TYPE_INT) {
			*((int *)ptr) = (int)(*((dtype *)value));
		}
		else if (pType == Parameter::TYPE_DOUBLE) {
			*((dtype *)ptr) = *((dtype *)value);
		}
		else if (pType == Parameter::TYPE_BOOL_ARRAY) {
			for (int i = 0; i < len; i++) {
				*((int *)ptr + i) = (int)(*((dtype *)value + i));
			}
		}
		else if (pType == Parameter::TYPE_INT_ARRAY) {
			for (int i = 0; i < len; i++) {
				*((int *)ptr + i) = (int)(*((dtype *)value + i));
			}
		}
		else if (pType == Parameter::TYPE_DOUBLE_ARRAY) {
			for (int i = 0; i < len; i++) {
				*((dtype *)ptr + i) = (dtype)(*((dtype *)value + i));
			}

			if ( ns == string("datapb") ) { /* Zero out the remaining part */
				for (int i = len; i < maxPBSize; i++)
					*((dtype *)ptr + i) = 0.0;
			}
		}
		else if (pType == Parameter::TYPE_PVOC_WARP) {
			pertCfg.addWarpCfg(*((double *)value), *((double *)value + 1), 
							   *((double *)value + 2), *((double *)value + 3), 
							   *((double *)value + 4));
			/* TODO: Clarify whether it is set or add */
		}
		else if (pType == Parameter::TYPE_SMN_RMS_FF) {
			if (nPars == 1) {
				p.rmsFF_fb[0]     = *(dtype *)value;
				p.rmsFF_fb[1]	  = p.rmsFF_fb[0];
				p.rmsFF_fb[2]     = 0.0;
				p.rmsFF_fb[3]	  = 0.0;
			}
			else if (nPars == 4) {
				for (int n = 0; n < nPars; n++) {
					p.rmsFF_fb[n] = *((dtype *)value + n);
				}
			}
			else {
				mexErrMsgTxt("ERROR: unexpected number of inputs for p.rmsFF_fb");
			}

			if ( (p.rmsFF_fb[0] < 0.0) || (p.rmsFF_fb[0] > 1.0) ||
				 (p.rmsFF_fb[1] < 0.0) || (p.rmsFF_fb[1] > 1.0) )
				 mexErrMsgTxt("Invalid values in rmsFF_fb[0] and/or rmsFF_fb[1]");

			rmsFF_fb_now = p.rmsFF_fb[0];
		}
	
		/* Additional internal parameter changes */
		if (ns == string("nfb")) {
			if ( bSet && ((p.nFB <= 0) || (p.nFB > maxNVoices)) ) {
				mexErrMsgTxt("Invalid value of nFB. nFB set to 1.");
				p.nFB = 1;
			}
		}
		else if (ns == string("framelen") || ns == string("ndelay")) {
			p.anaLen = p.frameShift + 2 * (p.nDelay - 1) * p.frameLen;
		}
		else if (ns == string("delayframes")) {
			for (int n = 0; n < maxNVoices && n < p.nFB; n++) {
				p.delayFrames[n]		= (int)(*((dtype *)value + n));
				if (p.delayFrames[n] < 0){
					TRACE("WARNING: delayFrames[%d] < 0. Set to 0 automatically.\n", n);
					p.delayFrames[n] = 0;
				}
				if (p.delayFrames[n] > maxDelayFrames){
					TRACE("WARNING: delayFrames[%d] > %d. Set to %d automatically.\n", n, maxDelayFrames, maxDelayFrames);
					p.delayFrames[n] = maxDelayFrames;
				}
			}
		}
		else if (ns == string("pvocframelen")) {
			
		}
		else if (ns == string("datapb")) {
			pbCounter = 0;
		}
		else if (ns == string("tsgint")) {
			tsgToneOnsets[0]=0;
			for (int n = 1; n < maxNTones; n++){
				tsgToneOnsets[n] = tsgToneOnsets[n - 1] + p.tsgInt[n-1];	//sec
			}
		}
		else if (ns == string("fb4gaindb")) {
			p.fb4Gain		= pow(10.0, p.fb4GainDB / 20);
		}

		p.frameShift	= p.frameLen / p.nWin;
		p.anaLen		= p.frameShift + 2 * (p.nDelay - 1) * p.frameLen;
		time_step		= (dtype)p.frameShift * 1000 / p.sr;	//SC Unit: ms
		p.minDetected	= p.nWin;

		/* Verbose mode */
		if (bVerbose) {
			ostringstream oss;

			oss << "Set parameter " << ns << " to: ";
		
			string valStr;
			if (pType == Parameter::TYPE_BOOL) {
				string tBoolStr;
				if (*((int *)ptr) > 0) {
					tBoolStr = "true";
				}
				else {
					tBoolStr = "false";
				}
				oss << tBoolStr;
			}
			else if (pType == Parameter::TYPE_INT || pType == Parameter::TYPE_INT_ARRAY) {
				if (len > 1)	oss << "[";

				for (int i = 0; i < len; i++) {
					if (len > maxDisplayElem && i == 3) {
						oss << "...";
						i = len - 2;
					}
					else {
						oss << *((int *)ptr + i);
					}

					if (len > 1) {
						if (i < len - 1)	
							oss << ", ";
						else				
							oss << "]";
					}
				}
			}
			else if (pType == Parameter::TYPE_DOUBLE || pType == Parameter::TYPE_DOUBLE_ARRAY) {
				if (len > 1)	oss << "[";
				for (int i = 0; i < len; i++) {
					if (len > maxDisplayElem && i == 3) {
						oss << "...";
						i = len - 2;
					}
					else {
						oss << *((dtype *)ptr + i);
					}

					if (len > 1) {
						if (i < len - 1)	
							oss << ", ";
						else				
							oss << "]";
					}
				}
			}
			oss << endl;

			string promptStr = oss.str();
			mexPrintf(promptStr.c_str());
		}


		/* Re-generate the formant tracker, if necessary */
		if (bRemakeFmtTracker) {
			if (fmtTracker)
				delete fmtTracker;

			try {
				fmtTracker = new LPFormantTracker(p.nLPC, p.sr, p.anaLen, nFFT, p.cepsWinWidth * p.bCepsLift, 
											      p.nTracks, p.aFact, p.bFact, p.gFact, p.fn1, p.fn2, 
												  static_cast<bool>(p.bWeight), p.avgLen);
			}
			catch (LPFormantTracker::initializationError) {
				mexErrMsgTxt("Failed to initialize formant tracker");
			}
			catch (LPFormantTracker::nLPCTooLargeError) {
				mexErrMsgTxt("Failed to initialize formant tracker due to a too larger value in nLPC");
			}
		}

		/* Re-initialize phase vocoder */
		if ( bRemakePVoc ) {
			PhaseVocoder::operMode t_operMode;
			if ( pVoc ) {
				t_operMode = pVoc[0].getMode();
				delete [] pVoc;
			}
			else {
				t_operMode = PhaseVocoder::operMode::PITCH_SHIFT_ONLY;
			}

			pVoc = new PhaseVocoder[p.nFB];
			for (int i = 0; i < p.nFB; ++i) {
				try {
					pVoc[i].config(t_operMode, p.nDelay, 
								   static_cast<dtype>(p.sr), p.frameLen, 
						    	   p.pvocFrameLen, p.pvocHop);
				}
				catch (PhaseVocoder::initializationError) {
					mexErrMsgTxt("Failed to initialize phase vocoder");
				}
			}
		}

		return NULL;
	}

	
}

void Audapter::setParam(const char *name, void * value, int nPars, bool bVerbose) {
	setGetParam(true, name, value, nPars, bVerbose, NULL);
}

void *Audapter::getParam(const char *name) {
	return setGetParam(false, name, NULL, 0, false, NULL);
}

void Audapter::queryParam(const char *name, mxArray **output) {
	int length;
	Parameter::paramType pType = params.checkParam(name);

	if (pType == Parameter::TYPE_NULL) {
		string errStr = "Unrecognized parameter: ";
		errStr += name;
		mexErrMsgTxt(errStr.c_str());
	}

	void *val;
	val = setGetParam(false, name, NULL, 0, false, &length);

	if (pType == Parameter::TYPE_BOOL || pType == Parameter::TYPE_BOOL_ARRAY
		|| pType == Parameter::TYPE_INT || pType == Parameter::TYPE_INT_ARRAY
		|| pType == Parameter::TYPE_DOUBLE || pType == Parameter::TYPE_DOUBLE_ARRAY) {
		
		*output = mxCreateDoubleMatrix(1, length, mxREAL);
		double *output_ptr = mxGetPr(*output);

		for (int i = 0; i < length; i++) {
			if (pType == Parameter::TYPE_BOOL || pType == Parameter::TYPE_BOOL_ARRAY) {
				output_ptr[i] = (double)(*((int *)val + i));
			}
			else if (pType == Parameter::TYPE_INT || pType == Parameter::TYPE_INT_ARRAY) {
				output_ptr[i] = (double)(*((int *)val + i));
			}
			else if (pType == Parameter::TYPE_DOUBLE || pType == Parameter::TYPE_DOUBLE_ARRAY) {
				output_ptr[i] = *((double *)val + i);
			}
		}
	}
	else if (pType == Parameter::TYPE_PVOC_WARP) {
		*output = mxCreateDoubleMatrix(0, 0, mxREAL);
		// TODO

		/*const int nFields = 6;
		pvocWarpAtom tWarpCfg = *((pvocWarpAtom *) val);

		const char *fields[nFields] = {"tBegin", "rate1", "dur1", "durHold", "rate2", "dur2"};
		mwSize nDims[2] = {1, 1};

		*output = mxCreateStructArray(2, nDims, nFields, fields);
		
		mxArray *ma;
		double *ma_ptr;
		for (int i = 0; i < nFields; i++) {
			ma = mxCreateDoubleMatrix(1, 1, mxREAL);
			ma_ptr = mxGetPr(ma);

			if (i == 0)
				ma_ptr[0] = tWarpCfg.tBegin;
			else if (i == 1)
				ma_ptr[0] = tWarpCfg.rate1;
			else if (i == 2)
				ma_ptr[0] = tWarpCfg.dur1;
			else if (i == 3)
				ma_ptr[0] = tWarpCfg.durHold;
			else if (i == 4)
				ma_ptr[0] = tWarpCfg.rate2;
			else if (i == 5)
				ma_ptr[0] = tWarpCfg.dur2;
			
			mxSetField(*output, i, fields[i], ma);
		}*/
	}
	else if (pType == Parameter::TYPE_SMN_RMS_FF) {
		*output = mxCreateDoubleMatrix(0, 0, mxREAL);
		//TODO
	}
	
}

const dtype* Audapter::getSignal(int & size) const {
	size = frame_counter*p.frameLen;	//SC Update size
	return signal_recorder[0];			//SC return the
}

const dtype* Audapter::getData(int & size, int & vecsize) const {
	size = data_counter;
	vecsize = 4 +2 * p.nTracks + 2 * 2 + p.nLPC + 8;
	return data_recorder[0];
}

const dtype* Audapter::getOutFrameBufPS() const {
	return outFrameBufPS[0];	//Marked
}

// Assumes: maxBufLen == 3*p.frameLen 
//		    
int Audapter::handleBuffer(dtype *inFrame_ptr, dtype *outFrame_ptr, int frame_size, bool bSingleOutputBuffer)	// Sine wave generator
{
	static bool during_trans = false;
	static bool maintain_trans = false;
	bool above_rms = false;
	bool bDoFmts;
	bool during_pitchShift = false;
	dtype sf1m, sf2m, loc, locfrac, mphi, mamp;
	int locint, n;

	/* Temporary buffer for holding output before duplexing into stereo */
	dtype outputBuf[maxFrameLen];

	static dtype time_elapsed=0;

	int fi=0, si=0, i0=0, offs=0, quit_hqr=0, indx=0, nZC=0, nZCp=0;	
	dtype rms_s, rms_p, rms_o, rms_fb, wei;
	int optr[maxNVoices]; //SC(2012/02/28) DAF
	
	char wavfn_in[256], wavfn_out[256];
	// ====== ~Variables for frequency/pitch shifting (smbPitchShift) ======

	if (frame_size != p.downFact * p.frameLen)	//SC This ought to be satisfied. Just for safeguard.
		return 1;

	for (i0 = 0; i0 < p.nTracks; i0++) {	//SC Initialize the frequencies and amplitudes of the formant tracks
		fmts[i0]=0;
		amps[i0]=0;
	}

	for (i0 = 0; i0 < 2; i0++) {			//SC Initialize the time derivative of formants (dFmts) and the shifted formants (sFmts)
		dFmts[i0] = 0;
		sFmts[i0] = 0;
	}

	/* downsample signal provided by soundcard */
	downSampSig(inFrame_ptr, inFrameBuf, p.frameLen, p.downFact, p.bDownSampFilt == 0);

	if (p.bRecord)		//SC Record the downsampled original signal
	{// recording signal in (downsampled)
		DSPF_dp_blk_move(&inFrameBuf[0], &signal_recorder[0][frame_counter * p.frameLen], p.frameLen);	//SC Copying
	}

	// push samples in oBuf and pBuf
	//SC audapter.oBuf: (downsampled) input, audapter.pBuf: preemphasized (downsampled) input
	DSPF_dp_blk_move(&oBuf[p.frameLen], &oBuf[0], 2 * (p.nDelay - 1) * p.frameLen);	
	DSPF_dp_blk_move(&pBuf[p.frameLen], &pBuf[0], 2 * (p.nDelay - 1) * p.frameLen);	//SC Pre-emphasized buffer shift to left

	// move inFrame into oBuf
	DSPF_dp_blk_move(&inFrameBuf[0], &oBuf[2 * (p.nDelay - 1) * p.frameLen], p.frameLen);

	// preemphasize inFrame and move to pBuf
	//SC Preemphasis amounts to an high-pass iir filtering, the output is pBuf
	//SC(2008/05/07)	
	preEmpFilter.filter(inFrameBuf, &pBuf[2 * (p.nDelay - 1) * p.frameLen], p.frameLen, 1.0);

	// load inBuf with p.frameLen samples of pBuf
	//SC Copy pBuf to inBuf. pBuf is the signal based on which LPC and other
	// analysis will be done. inBuf is the signal that will be shifted (if in shifting mode).
	DSPF_dp_blk_move(&pBuf[(p.nDelay - 1) * p.frameLen], &inBuf[0], p.frameLen);

	// move inBuf to outBuf  (will be overwritten when filtering)
	DSPF_dp_blk_move(&inBuf[0], &outBuf[0], p.frameLen);

	for(fi = 0; fi < p.nWin; fi++)// each incoming frame is divided in nwin windows
	{
		during_trans=false;
		gtot[fi]=1;			// initialize gain factor				
		si=fi*p.frameShift;// sample index
		rms_o=sqrt(DSPF_dp_vecsum_sq(&oBuf[(p.nDelay-1)*p.frameLen+si],p.frameShift)/((dtype)p.frameShift)); //short time rms of current window
		rms_s=calcRMS1(&oBuf[(p.nDelay-1)*p.frameLen+si], p.frameShift); // Smoothed RMS of original signal 
		rms_p=calcRMS2(&pBuf[(p.nDelay-1)*p.frameLen+si], p.frameShift); // RMS preemphasized (i.e., high-pass filtered) signal, also smoothed	

		rms_fb = calcRMS_fb(&oBuf[(p.nDelay - 1) * p.frameLen + si], p.frameShift, rms_s > p.dRMSThresh); // Smoothed RMS of original signal

		rms_ratio = rms_s / rms_p; // rmsratio indicates if there is a fricative around here...	

		//SC-Mod(2008/01/11)		
		//SC Notice that the identification of a voiced frame requires, 1) RMS of the orignal signal is large enough,
		//SC	2) RMS ratio between the orignal and preemphasized signals is large enough
		if (rms_s >= p.dRMSThresh * 2) {			
			above_rms=(isabove(rms_s,p.dRMSThresh) && isabove(rms_ratio,p.dRMSRatioThresh/1.3));
		}
		else{
			above_rms=(isabove(rms_s,p.dRMSThresh) && isabove(rms_ratio,p.dRMSRatioThresh));
		}

		/* TODO: Implement bDoFmts */
		/* if (ostTab.n == 0) { */
			bDoFmts = above_rms;
		/* }
		else {
			bDoFmts = above_rms & (stat >= 0);
		} */

		/*if(above_rms)*/
		if (bDoFmts) /* DEBUG: Ad hoc */
		{
			if (p.bWeight)	//SC bWeight: weighted moving averaging of the formants //Marked
				wei=rms_o; // weighted moving average over short time rms
			else
				wei=1; // simple moving average

			fmtTracker->procFrame(&pBuf[si], rms_o, amps, wmaPhis, bw, fmts);

			//SC Convert to mel. The perturbation field variables (F2Min, F2Max, pertF2, etc.) are all in mel. 			
			if (p.bMelShift){
				f1m = hz2mel(fmts[0]);
				f2m = hz2mel(fmts[1]);
			}
			else{
				f1m = fmts[0];
				f2m = fmts[1];
			}

			getDFmt(&fmts[0], &dFmts[0], time_elapsed); // formant derivatives (note utilized in the current version, but may be useful in the future)

			if (p.bDetect)	//SC bDetect: to detect transition?
				during_trans=detectTrans(&fmts[0], &dFmts[0], data_counter, time_elapsed); // [a] [i] transition detection
			else
				during_trans=false;

			time_elapsed+=time_step;

			//SC(2009/02/02)
			bLastFrameAboveRMS = 1;
		}
		else
		{
			time_elapsed = 0.0;
			//weiVec[circ_counter]=0;  // Marked, TODO
			for (i0 = 0; i0 < p.nTracks; i0++) {		//SC Put zeros in formant frequency and amplitude for unvoiced portion
				amps[i0] = 0.0f;
				orgPhis[i0] = 0.0f;
				bw[i0] = 0.0f;
				wmaPhis[i0] = 0.0f;
				wmaR[i0] = 0.0f;

				fmts[i0]=0;
			}

			/* Reset roots in formant tracker */
			if (fmtTracker)
				fmtTracker->postSupraThreshReset();

			bLastFrameAboveRMS=0;
		}

		a_rms_o[data_counter] = rms_s;
		rmsSlopeN = (int)(rmsSlopeWin / ((dtype)p.frameLen / (dtype)p.sr));
		calcRMSSlope();

		stat = ostTab.osTrack(stat, data_counter, frame_counter, 
							  static_cast<double>(a_rms_o[data_counter]), static_cast<double>(a_rms_o_slp[data_counter]), static_cast<double>(rms_ratio), 
							  data_recorder[1], 
							  static_cast<double>(p.frameLen) / static_cast<double>(p.sr));
		
		if (p.bShift)
		{
			if (pertCfg.n == 0) {
				/*during_trans = false;*/
			}
			else {
				during_trans = (pertCfg.fmtPertAmp[stat] != 0);
			}

			if(during_trans && bDoFmts) // Determine whether the current point in perturbation field
			{// yes : windowed deviation over x coordinate
				loc=locateF2(f2mp);	// Interpolation (linear)								
				locint=(int)floor(loc);
				locfrac=loc-locint;
				
				/* That using ost and pcf files overrides the perturbatoin field 
					specified with pertF2, pertAmp, pertPhi. */
				if (pertCfg.n > 0) {
					mamp = pertCfg.fmtPertAmp[stat];
					mphi = pertCfg.fmtPertPhi[stat];
				}
				else {
					mamp=p.pertAmp[locint]+locfrac*(p.pertAmp[locint+1]-p.pertAmp[locint]);	// Interpolaton (linear)
					mphi=p.pertPhi[locint]+locfrac*(p.pertPhi[locint+1]-p.pertPhi[locint]);
				}

				if (!p.bRatioShift){	// Absoluate shift					
					sf1m = f1m + mamp * cos(mphi);	// Shifting imposed
					sf2m = f2m + mamp * sin(mphi);
				}
				else{	// Ratio shift					
					//mamp=p.pertAmp[locint]+locfrac*(p.pertAmp[locint+1]-p.pertAmp[locint]);					
					sf1m = f1m * (1 + mamp * cos(mphi));
					sf2m = f2m * (1 + mamp * sin(mphi));
				}

				if (p.bMelShift){
					newPhis[0]=mel2hz(sf1m)/p.sr*2*M_PI;	// Convert back to Hz
					newPhis[1]=mel2hz(sf2m)/p.sr*2*M_PI;
				}
				else{
					newPhis[0]=sf1m/p.sr*2*M_PI;
					newPhis[1]=sf2m/p.sr*2*M_PI;
				}
			}
			else
			{// no : no force applied
				maintain_trans=false;
				during_trans=false;	
				newPhis[0]=wmaPhis[0];	// No shifting
				newPhis[1]=wmaPhis[1];
			}

			if(during_trans || maintain_trans)// directly after transition
			{
				for (i0=0;i0<2;i0++)
				{
					sFmts[i0]=newPhis[i0]*p.sr/(2*M_PI); // shifted fotmants for recording
				}

				formantShiftFilter(&inBuf[si],&outBuf[si],&wmaPhis[0],&newPhis[0],&amps[0],p.frameShift); // f1 f2 filtering 
				gtot[fi]=getGain(&amps[0],&wmaPhis[0],&newPhis[0],p.nFmts); // gain factor calculation

			}
			else// no transition
			{
	 
				if(above_rms)				
					formantShiftFilter(&inBuf[si],&fakeBuf[0],&wmaPhis[0],&wmaPhis[0],&amps[0],p.frameShift);
			}

		}
		// data recording
		data_recorder[0][data_counter] = frame_counter * p.frameLen + si + 1;// matlab intervals
		data_recorder[1][data_counter] = rms_s;
		data_recorder[2][data_counter] = rms_p;
		data_recorder[3][data_counter] = rms_o;

		offs = 4;
		//SC Write formant frequencies and amplitudes to data_recorder
		for (i0 = 0; i0 < p.nTracks; i0++)
		{
			data_recorder[i0 + offs][data_counter] = fmts[i0];
			data_recorder[i0 + offs + p.nTracks][data_counter] = amps[i0];
		}
		offs += 2 * p.nTracks;

		//SC Write time derivative of formants and shifted formants to data_recorder
		for (i0 = 0; i0 < 2; i0++)
		{
			data_recorder[i0 + offs][data_counter] = dFmts[i0];
			data_recorder[i0 + offs + 2][data_counter] = sFmts[i0];
		}
		offs += 4;

		//SC Write the LPC coefficients to data_recorder
		for (i0=0;i0<p.nLPC+1;i0++)
		{
			data_recorder[i0+offs][data_counter]		= fmtTracker->lpcAi[i0];
		}
		offs += p.nLPC + 1;

		//SC(10/18/2012): Write a_rms_o_slp (RMS slope) to data_recorder	
		data_recorder[offs][data_counter] = a_rms_o_slp[data_counter];

		offs += 1;
		data_recorder[offs][data_counter] = stat;

	}
	
	// gain adaption: optional
	//SC Notice that gain adaptation is done after formant shifts
	if (p.bGainAdapt) 
		nZC = gainAdapt(&outBuf[0], &gtot[0], p.frameLen, p.frameShift); // apply gain adaption between zerocrossings

	//SC(2012/10/19)
	nZCp = gainPerturb(&outBuf[0], &gtot[0], p.frameLen, p.frameShift);
		
	if (!p.bBypassFmt) { /* Not bypassing LP formant tracking and shifting */
		// deemphasize last processed frame and send to outframe buffer
		//SC(2008/05/07)
		deEmpFilter.filter(outBuf, &outFrameBuf[outFrameBuf_circPtr], p.frameLen, 1.0);
		//DSPF_dp_blk_move(&outBuf[0],&outFrameBuf[0],p.frameLen);

		if (rms_s > p.rmsClipThresh && p.bRMSClip == 1){	//SC(2009/02/06) RMS clipping protection
			for(n = 0; n < frame_size / p.downFact; n++){
				outFrameBuf[n]=0;
			}
		}
	}
	else {
		DSPF_dp_blk_move(&inFrameBuf[0], &outFrameBuf[outFrameBuf_circPtr], p.frameLen);
	}

	// === Frequency/pitch shifting code ===
	dtype xBuf[max_nFFT];

	//SCai (10/19/2012): determine the amount of pitch and intensity shift based on the OST stat number
	// Note: we will override p.pitchShiftRatio[0] if pertCfg.n > 0d
	if (pertCfg.n > 0) {
		if (stat < pertCfg.n) {
			p.pitchShiftRatio[0] = pow(2.0, pertCfg.pitchShift[stat] / 12.0);
			intShiftRatio = pow(10, pertCfg.intShift[stat] / 20.0);
		}
	}
	
	//Marked
	dtype ms_in = 0.0; // DEBUG: amp
	dtype ms_out = 0.0; // DEBUG: amp 
	dtype out_over_in = 0.0; // DEBUG: amp
	dtype ss_inBuf = 0.0; // DEBUG: amp
	dtype ss_outBuf = 0.0; // DEBUG: amp
	dtype ss_anaMagn = 0.0; // DEBUG: amp
	dtype ss_synMagn = 0.0;// DEBUG: amp

	int isPvocFrame = (((frame_counter_nowarp - (p.nDelay - 1)) % (p.pvocHop / p.frameLen) == 0) && 
					   (frame_counter_nowarp - (p.nDelay - 1) >= p.pvocFrameLen / p.frameLen));

	if (p.bPitchShift == 1){		// PVOC Pitch shifting
		//////////////////////////////////////////////////////////////////////
		if (isPvocFrame == 0) {
			duringPitchShift = duringPitchShift_prev;

			if (frame_counter_nowarp - (p.nDelay - 1) < p.pvocFrameLen / p.frameLen) {
				duringPitchShift = false;

				if ( pertCfg.n > 0)
					p.pitchShiftRatio[0] = 1.0;
			}
		}
		else {			
			if (outFrameBuf_circPtr - p.pvocFrameLen >= 0)
				DSPF_dp_blk_move(&outFrameBuf[outFrameBuf_circPtr - p.pvocFrameLen], xBuf, p.pvocFrameLen);
			else{ // Take care of wrapping-around
				DSPF_dp_blk_move(&outFrameBuf[0], &xBuf[p.pvocFrameLen - outFrameBuf_circPtr], outFrameBuf_circPtr);
				DSPF_dp_blk_move(&outFrameBuf[internalBufLen - p.pvocFrameLen + outFrameBuf_circPtr], 
								    &xBuf[0], 
									p.pvocFrameLen - outFrameBuf_circPtr);
			}

			// --- Time warping preparation ---
			dtype t0 = (dtype)(frame_counter - (p.nDelay - 1)) * p.frameLen / p.sr;
			dtype t1;

			// --- ~Time warping preparation ---
			/* Time warping, overrides pitch shifting */
			int ifb_max;
			if (pertCfg.n > 0) /* PCF overrides multivoice mode */
				ifb_max = 0;
			else
				ifb_max = p.nFB - 1;

			for (int ifb = 0; ifb <= ifb_max; ++ifb) {
				duringTimeWarp = pertCfg.procTimeWarp(stat, ostTab.statOnsetIndices, 
													  p.nDelay, static_cast<double>(p.frameLen) / static_cast<double>(p.sr), 
									    			  t0, t1);
			
				/* Call phase vocoder */
				try {
					if (pVoc[ifb].getMode() == PhaseVocoder::TIME_WARP_ONLY) {
						dtype warp_t;
						if (duringTimeWarp) {
							warp_t = t1 - t0; /* Expected to be negative */	
						}
						else {
							warp_t = 0.0;
						}
					
						pVoc[ifb].procFrame(xBuf, warp_t);
					}
					else {
						if (pertCfg.pitchShift && stat < pertCfg.n) {						
							pVoc[ifb].procFrame(xBuf, pertCfg.pitchShift[stat]);
						}
						else if ( (pertCfg.n == 0) && (p.pitchShiftRatio[ifb] != 1.0) ) {
							pVoc[ifb].procFrame(xBuf, (log(p.pitchShiftRatio[ifb]) / log(2.0)) * 12.0);
						}
						else {
							pVoc[ifb].procFrame(xBuf, 0.0);
						}					
					}
				}
				catch (PhaseVocoder::timeWarpFuturePredError) {
					mexErrMsgTxt("Error occurred during phase-vocoder "
								 "time warping: predicting the future is impossible");
				}
				catch (PhaseVocoder::fixedPitchShiftNotSpecifiedErr) {
					mexErrMsgTxt("Phase vocoder error: "
								 "Fixed pitch shift not specified under mode "
								 "TIME_WARP_WITH_FIXED_PITCH_SHIFT");
				}

				duringTimeWarp_prev = duringTimeWarp;

				// --- Accumulate to buffer ---
				for (i0 = 0; i0 < p.pvocFrameLen; i0++){
					/* Using pVoc results */
					outFrameBufPS[ifb][(outFrameBuf_circPtr + i0) % (internalBufLen)] = 
						outFrameBufPS[ifb][(outFrameBuf_circPtr + i0) % (internalBufLen)] + 
						pVoc[ifb].ftBuf2[2 * i0];
				}			

				// Front zeroing
				for (i0 = 0; i0 < p.pvocHop; i0++) {
					outFrameBufPS[ifb][(outFrameBuf_circPtr + p.pvocFrameLen + i0) % (internalBufLen)] = 0.;
				}

				// Back Zeroing
				for (i0 = 1; i0 <= p.pvocHop; i0++){ // Ad hoc alert!
					outFrameBufPS[ifb][(outFrameBuf_circPtr - p.delayFrames[ifb] * p.frameLen - i0) % (internalBufLen)] = 0.;						
				}			
			}
		}

		duringPitchShift_prev = duringPitchShift;
	}
	else{
		duringPitchShift = false;
		//p.pitchShiftRatio[0] = 1.0;
		for (i0 = 0; i0 < p.nFB; i0++)
			DSPF_dp_blk_move(&outFrameBuf[outFrameBuf_circPtr], &outFrameBufPS[i0][outFrameBuf_circPtr], p.frameLen);
	}


	offs++;
	data_recorder[offs][data_counter] = p.pitchShiftRatio[0];

	offs++;
	if (isPvocFrame) {
		data_recorder[offs][data_counter] = ms_in;
	}
	else {
		if (data_counter > 0) {
			data_recorder[offs][data_counter] = data_recorder[offs][data_counter - 1];
		}
	}

	// === ~Frequency/pitch shifting code ===

	for (int h0 = 0; h0 < p.nFB; h0++) {
		if (outFrameBuf_circPtr - p.delayFrames[h0] * p.frameLen > 0) {
			optr[h0] = outFrameBuf_circPtr - p.delayFrames[h0] * p.frameLen;
		}
		else {
			optr[h0] = outFrameBuf_circPtr - p.delayFrames[h0] * p.frameLen + (internalBufLen);
		}
	}

	//SCai(2012/09/08) Blueshift: Sum to cumulative buffer (p.nFB)
	for (int n0 = 0; n0 < p.frameLen; n0++) {
		outFrameBufSum[n0 + p.pvocFrameLen - p.frameLen] = 0;
		for (int m0 = 0; m0 < p.nFB; m0++) {
			if (p.mute[m0] == 0)
				outFrameBufSum[n0 + p.pvocFrameLen - p.frameLen] += outFrameBufPS[m0][optr[m0] + n0] * p.gain[m0];
		}
	}

	if (p.bPitchShift == 1 && p.bPvocAmpNorm == 1) {
		/* Circularly move the non-normalized cumulative buffer: outFrameBufSum2 */
		DSPF_dp_blk_move(outFrameBufSum2 + p.frameLen, 
						 outFrameBufSum2, 
						 p.pvocFrameLen - p.frameLen);

		/* Move to outFrameBufSum2: the non-normalized cumulative buffer */	
		DSPF_dp_blk_move(outFrameBufSum + p.pvocFrameLen - p.frameLen, 
						 outFrameBufSum2 + p.pvocFrameLen - p.frameLen, 
						 p.frameLen);
	
		// DEBUG: amp normalization
		int nzCnt = 0;
		for (int n0 = 0; n0 < p.pvocFrameLen; n0++) {
			if (outFrameBufSum2[n0] != 0) {
				ms_out += outFrameBufSum2[n0] * outFrameBufSum2[n0];
				nzCnt++;
			}
		}
		ms_out /= (dtype) nzCnt;

		offs++;
		data_recorder[offs][data_counter] = ms_out;

		if (isPvocFrame == 1
			//&& (frame_counter_nowarp - (p.nDelay - 1) > p.pvocFrameLen / p.frameLen)
			&& (nzCnt == p.pvocFrameLen)) {
				out_over_in = ms_out / ms_in;
				amp_ratio = sqrt(out_over_in);
		}

		dtype t_amp_ratio;
		dtype trans_n;

		if (p.pvocAmpNormTrans <= p.frameLen)
			trans_n = p.pvocAmpNormTrans;
		else
			trans_n = p.frameLen;

		for (int n0 = 0; n0 < p.frameLen; n0++) {
			for (int m0 = 0; m0 < p.nFB; m0++) {
				if (n0 < trans_n) {
					t_amp_ratio = amp_ratio_prev + (amp_ratio - amp_ratio_prev) / trans_n * n0;
				}
				else {
					t_amp_ratio = amp_ratio;
				}

				outFrameBufSum[n0 + p.pvocFrameLen - p.frameLen] /= t_amp_ratio;
			}
		}

		amp_ratio_prev = amp_ratio;
	}

	offs++;
	data_recorder[offs][data_counter] = ms_out;

	data_counter++;
	circ_counter= data_counter % maxPitchLen;

	if (p.fb == 0) {	// Mute
		for(n = 0;n < p.frameLen; n++){
			outFrameBufSum[n + p.pvocFrameLen - p.frameLen] = 0;
		}
	}
	else if (p.fb >= 2 && p.fb <= 4) {
		for(n = 0;n < p.frameLen; n++) {
			if (p.fb == 2)	// noise only
				outFrameBufSum[n + p.pvocFrameLen - p.frameLen] = data_pb[pbCounter];
			else if (p.fb == 3)	// voice + noise				
				outFrameBufSum[n + p.pvocFrameLen - p.frameLen] = outFrameBufSum[n + p.pvocFrameLen - p.frameLen] + data_pb[pbCounter] * p.fb3Gain;
			else if (p.fb == 4)	// Speech-modulated noise	
				outFrameBufSum[n + p.pvocFrameLen - p.frameLen] = data_pb[pbCounter] * rms_fb * p.fb4Gain * p.dScale;

			pbCounter += p.downFact;
			if (pbCounter >= maxPBSize)
				pbCounter -= maxPBSize;
		}		
	}

	if (p.bRecord)
	{// recording signal output
		//DSPF_dp_blk_move(&outFrameBufPS[optr], &signal_recorder[1][frame_counter*p.frameLen], p.frameLen);
		DSPF_dp_blk_move(outFrameBufSum + p.pvocFrameLen - p.frameLen, 
						 &signal_recorder[1][frame_counter * p.frameLen], 
						 p.frameLen);
	}

	// Upsample signal, scale and send to sound card buffer
	//SC(2012/02/28) For DAF: use a past frame for playback, if p.delayFrames > 0
	/*upSampSig(upSampFilter.b, upSampFilter.a, outFrameBufSum + p.pvocFrameLen - p.frameLen, 
			  upSampFilter.buff, outputBuf, upSampFilter.delay, p.frameLen * p.downFact, 
			  upSampFilter.filtLen, p.downFact, p.dScale);*/
	upSampSig(upSampFilter, outFrameBufSum + p.pvocFrameLen - p.frameLen, 
			  outputBuf, p.frameLen * p.downFact, 
			  p.downFact, p.dScale);

	outFrameBuf_circPtr += p.frameLen;
	if (outFrameBuf_circPtr >= internalBufLen){
		//mexPrintf("outFrameBuf_circPtr = %d --> 0\n", outFrameBuf_circPtr);
		outFrameBuf_circPtr = 0;
	}
	
	//SC(2008/06/20) Blend with noise, if fb=2 or 3; Mute, if fb=0;
	/* if (p.fb==3){	// voice + noise
		for(n=0;n<frame_size;n++){
			outputBuf[n] = outputBuf[n]+data_pb[pbCounter];
			pbCounter=pbCounter+1;
			if (pbCounter==maxPBSize){
				pbCounter=0;
			}
		}
	}
	else if (p.fb==2){	// noise only
		for(n=0;n<frame_size;n++){
			outputBuf[n] = data_pb[pbCounter];
			pbCounter=pbCounter+1;
			if (pbCounter==maxPBSize){
				pbCounter=0;
			}
		}
	}
	else if (p.fb==0){	// Mute
		for(n=0;n<frame_size;n++){
			outputBuf[n] = 0;
		}
	} */

	/* Duplex into stereo */
	if (duringPitchShift)
		duringPitchShift = duringPitchShift; // DEBUG

	if (bSingleOutputBuffer) {
		for (n = 0; n < frame_size; n++) {
			outFrame_ptr[n] = outputBuf[n];
		}
	}
	else {
		if (p.stereoMode == 1) {	/* Left-right audio identical */
			for (n = 0; n < frame_size; n++)
				outFrame_ptr[2 * n] = outFrame_ptr[2 * n + 1] = outputBuf[n];
			}
		else if (p.stereoMode == 0) { /* Left audio only */
			for (n = 0; n < frame_size; n++) {
				outFrame_ptr[2 * n] = outputBuf[n];
				outFrame_ptr[2 * n + 1] = 0.0;
			}
		}
		else if (p.stereoMode == 2) { /* Left audio; right simulated TTL */
			for (n = 0; n < frame_size; n++) {
				outFrame_ptr[2 * n] = outputBuf[n];
				outFrame_ptr[2 * n + 1] = 0.99 * (dtype) duringPitchShift;
			}
		}
		else {

		}
	}

	//SC(2008/06/22) Impose the onset and offset ramps, mainly to avoid the unpleasant "clicks" at the beginning and end
	int nMult = bSingleOutputBuffer ? 1 : 2;
	if ( (p.trialLen > 0.0) && 
		 (static_cast<dtype>(frame_counter) * static_cast<dtype>(p.frameLen) / static_cast<dtype>(p.sr) > p.trialLen) ) {		
		for (n = 0; n < frame_size * nMult; ++n) {
			outFrame_ptr[n] = 0.0;
		}

	}
	if ( (p.trialLen > 0.0) && (p.rampLen > 0.0) && 
		 (static_cast<dtype>(frame_counter) * static_cast<dtype>(p.frameLen) / static_cast<dtype>(p.sr) <= p.rampLen) ) {
		for (n = 0; n < frame_size * nMult; ++n)
			outFrame_ptr[n] *= static_cast<dtype>((frame_counter - 1) * frame_size + n) / static_cast<dtype>(p.sr) / static_cast<dtype>(p.downFact) / p.rampLen;
	}
	if ( (p.trialLen > 0.0) && (p.rampLen > 0.0) && 
		 (static_cast<dtype>(frame_counter) * static_cast<dtype>(p.frameLen) / static_cast<dtype>(p.sr) >= p.trialLen - p.rampLen) ) {
		for (n = 0; n < frame_size * nMult; ++n)
	 		outFrame_ptr[n] *= (p.trialLen - static_cast<dtype>((frame_counter - 1) * frame_size + n) / static_cast<dtype>(p.sr) / static_cast<dtype>(p.downFact)) / p.rampLen;
	}
	

	frame_counter++;
	frame_counter_nowarp++;

	if (((frame_counter)*p.frameLen>=maxRecSize) || ((data_counter+p.nWin)>=maxDataSize)) //avoid segmentation violation
	{	
		//mexPrintf("data_counter = %d; frame_counter == %d --> 0.\n", data_counter, frame_counter);
		frame_counter=0;
		data_counter=0;
		
		//SC(2012/09/24) Write to wav file
		//Test threading
		sprintf_s(wavfn_in, "%sinput_%.3d.wav", wavFileBase, dataFileCnt);
		sprintf_s(wavfn_out, "%soutput_%.3d.wav", wavFileBase, dataFileCnt);

		thrwws.pThis = this;
		thrwws.wavfn_in = wavfn_in;
		thrwws.wavfn_out = wavfn_out;

		/* *** TODO: Make optional ***
		_beginthreadex(NULL, // No security
					   0, // Let OS determine stack size
					   Audapter::thrStatEntPnt, 
					   (void *)(&thrwws), 
					   0, // Running
					   NULL);
		*/
	}

	return 0;
}

int Audapter::handleBufferSineGen(dtype *inFrame_ptr, dtype *outFrame_ptr, int frame_size)	// Sine wave (pure tone) generator
{
	const dtype dt = 1.0 / p.sr / p.downFact;
	
	for(int n = 0; n < frame_size; ++n) {
		// outFrame_ptr[n]=p.wgAmp*sin(2*M_PI*p.wgFreq*p.wgTime);
		outFrame_ptr[n * 2] = outFrame_ptr[n * 2 + 1] = p.wgAmp*sin(2*M_PI*p.wgFreq*p.wgTime);
		p.wgTime=p.wgTime+dt;
	}

	return 0;
}

int Audapter::handleBufferWavePB(dtype *inFrame_ptr, dtype *outFrame_ptr, int frame_size)	// Wave playback
{	// Wave playback
	int n;

	for(n=0;n<frame_size;n++){
		outFrame_ptr[n]=data_pb[pbCounter];
		pbCounter=pbCounter+1;
		if (pbCounter==maxPBSize){
			pbCounter=0;
		}

	}

	return 0;
}




int Audapter::getDFmt(dtype *fmt_ptr,dtype *dFmt_ptr, dtype time)
{// calculates the formant derivatives [Hz/ms] smoothed with forgetting factor 
	int i0=0;

	for(i0=0;i0<2;i0++)
	{

		if (time< 20)
			deltaFmt[i0]=0;
		else
			deltaFmt[i0]=(fmt_ptr[i0]-lastFmt[i0])/time_step;//0.5625;

		if (fabs(deltaFmt[i0])>p.maxDelta)
			deltaFmt[i0]=p.maxDelta*(dtype)sign(deltaFmt[i0]);

		deltaMaFmt[i0]=(1-p.dFmtsFF)*deltaFmt[i0]+p.dFmtsFF*deltaMaFmt[i0];
		dFmt_ptr[i0]=deltaMaFmt[i0];
		lastFmt[i0]=fmt_ptr[i0];
	
	}

	return 0; 
}


bool Audapter::detectTrans(dtype *fmt_ptr, dtype *dFmt_ptr,int datcnt, dtype time)
{ // detects a-i transition using f1 and f2 derivatives in time	
	static bool btransition=false;		//SC Note these are static addresses. Output argument: btransition.
	//static bool bIsTrans=false;			//SC bIsTrans is used as an internal state variable.
	//static int detect_counter = 0;			

	if (p.bShift){
		if (!p.transDone){
			f2mp=f2m;
			if (f2mp>=p.F2Min && f2mp<=p.F2Max && f1m>=p.F1Min && f1m<=p.F1Max 
				&& ((p.LBk<=0 && f1m*p.LBk+p.LBb<=f2m) || (p.LBk>0 && f1m*p.LBk+p.LBb>=f2m))){
				btransition=true;
				p.transCounter++;
			}
			else{
				btransition=false;				
				if (p.transCounter>=p.minVowelLen){
					p.transDone=true;
				}
				p.transCounter=0;
			}			
		}
		else{
			btransition=false;
			p.transCounter=0;
		}
	}
	else{
		btransition=false;
		p.transCounter=0;
	}	
	return btransition;
}



dtype Audapter::getGain(dtype * r, dtype * ophi,dtype * sphi, int nfmts)
{// this routine calculates the gain factor used by the routine gainAdapt to compensate the formant shift
//SC-Mod(2008/01/05) arguments xiff0 and xiff1: for intensity correction during formant shifts
	int i0;
	dtype prod=1;
	
	for (i0 = 0; i0 < nfmts; i0++)
		prod  *= (1-2*r[i0]*cos(sphi[i0])+pow(r[i0],2.0))/(1-2*r[i0]*cos(ophi[i0]) + pow(r[i0],2.0));
	
	return prod;
}

int Audapter::gainAdapt(dtype *buffer,dtype *gtot_ptr,int framelen, int frameshift)
{// this routine applies the gain factors collected in the vector gTot to the signal
 // for each window (nwin windows in one frame) the function finds the first zerocrossing
 // and updates the gain factor used to scale this window
 // gainadaption is done before deemphasis for two reasons:
	//1: more zerocrossings in the preemphasized signal
	//2: abrupt gain changings are more likely to introduce high frequency noise which will be filtered by the deemphasis filter
 // if no zerocrossing is found in the current window the gain factor will not be updated in this frame
 // assumtpion: zerocrossing is present in nearly each window (can be verified : the value returned by this fuction should equal nWin)
 // gain factor only changes slowely
	int i,index=0,updated=0;
	static dtype gain=1;// 1=no gain adaption =0dB
	static dtype lastSample=buffer[0];// used for continuous zero-crossing finding	

	bool armed=true;

	if (lastSample * buffer[0] <= 0)// zero crossing at first sample ?
	{
		gain=gtot_ptr[0]; // update gain
		armed=false; // zero finding disabled untill next win
		updated++; // number of times gain has been updated (should be equal to nwin)
	}

	for (i=0;i<framelen-1;i++)// scan complete frame 
	{
		
		buffer[i]=gain * buffer[i];// apply gain 

		if (i>=(index+1)*frameshift-1) // next win ?
		{
			index++; // next win
			armed=true; // enable next zero search
		}


		if (armed && (buffer[i] * buffer[i+1] <= 0))// zero crossing in win ? 
		{
			gain=gtot_ptr[index]; // update gain
			armed=false; // zero finding disabled untill next win
			updated++; // number of times gain has been updated (should be equal to nwin)
		}

	
	}
	lastSample=buffer[framelen-1]; //store last sample for next function call
	buffer[framelen-1]=gain * buffer[framelen-1];//last sample
	return updated;

	
}




void Audapter::formantShiftFilter(dtype *xin_ptr, dtype* xout_ptr, 
								  dtype *oldPhi_ptr, dtype *newPhi_ptr, dtype *r_ptr, 
								  const int size) {
	// filter cascading two biquad IIR filters 
	// coefficients for the first filter (f1 shift) NOTE: b_filt1[0]=1 (see initilization)
	b_filt1[1]=-2*r_ptr[0]*cos(oldPhi_ptr[0]);
	b_filt1[2]= r_ptr[0]*r_ptr[0]; 	
	a_filt1[1]=-2*r_ptr[0]*cos(newPhi_ptr[0]);  
	a_filt1[2]= r_ptr[0]*r_ptr[0]; 

	shiftF1Filter.setCoeff(3, a_filt1, 3, b_filt1);

	// coefficients for the second filter (f2 shift) NOTE: b_filt2[0]=1 (see initilization)
	b_filt2[1]=-2*r_ptr[1]*cos(oldPhi_ptr[1]);
	b_filt2[2]= r_ptr[1]*r_ptr[1];
	a_filt2[1]=-2*r_ptr[1]*cos(newPhi_ptr[1]);  
	a_filt2[2]= r_ptr[1]*r_ptr[1];

	shiftF2Filter.setCoeff(3, a_filt2, 3, b_filt2);

	shiftF1Filter.filter(xin_ptr, filtbuf, size);
	shiftF2Filter.filter(filtbuf, xout_ptr, size);
}

// Calculate rms of buffer
dtype Audapter::calcRMS1(const dtype *xin_ptr, int size)
{
	//SC rmsFF: RMF forgetting factor, by default equals 0.9.
	ma_rms1=(1-p.rmsFF)*sqrt(DSPF_dp_vecsum_sq(xin_ptr,size)/(dtype)size)+p.rmsFF*ma_rms1;
	return ma_rms1 ;
}

dtype Audapter::calcRMS2(const dtype *xin_ptr, int size)
{
	ma_rms2=(1-p.rmsFF)*sqrt(DSPF_dp_vecsum_sq(xin_ptr,size)/(dtype)size)+p.rmsFF*ma_rms2;
	return ma_rms2 ;
}

dtype Audapter::calcRMS_fb(const dtype *xin_ptr, int size, bool above_rms)
{
	//SC rmsFF: RMF forgetting factor, by default equals 0.9.
	const dtype frameDur = (float)p.frameLen / (float)p.sr;
	int cntThresh;
	dtype ff_inc, ff_dec;
	
	if (p.rmsFF_fb[2] > 0.0) {
		ff_inc = (p.rmsFF_fb[1] - p.rmsFF_fb[0]) / (int) (p.rmsFF_fb[2] / frameDur);
		ff_dec = (p.rmsFF_fb[0] - p.rmsFF_fb[1]) / (int) (p.rmsFF_fb[3] / frameDur);

		/* Status tracking */
		if (fb4_status == 0) {
			if (above_rms) {
				fb4_status = 1;
				fb4_counter = 0;				
			}
		}
		else if (fb4_status == 1) {
			if (above_rms) {
				fb4_counter++;
				rmsFF_fb_now += ff_inc;

				cntThresh = (int) (p.rmsFF_fb[2] / frameDur);
				if (fb4_counter >= cntThresh) {
					fb4_status = 2;
					rmsFF_fb_now = p.rmsFF_fb[1];
				}
			}
			else {
				fb4_status = 0;
				fb4_counter = 0;
				rmsFF_fb_now = p.rmsFF_fb[0];
			}
		}
		else if (fb4_status == 2) {
			if (!above_rms) {
				fb4_status = 3;
				fb4_counter = 0;
			}
		}
		else if (fb4_status == 3) {
			if (!above_rms) {
				fb4_counter++;

				cntThresh = (int) (p.rmsFF_fb[3] / frameDur);
				if (fb4_counter >= cntThresh) {
					fb4_status = 4;
				}
			}
			else {
				fb4_status = 2;
				fb4_counter = 0;
			}
		}
		else if (fb4_status == 4) { /* Voice ended: now ramp down the kernel size */
			/*if ((p.rmsFF_fb[1] - p.rmsFF_fb[0]) * (rmsFF_fb_now - p.rmsFF_fb[0]) >= 0.0) {
				rmsFF_fb_now += ff_dec;
			}*/
			if (rmsFF_fb_now != p.rmsFF_fb[0]) {
				rmsFF_fb_now = p.rmsFF_fb[0];
				fb4_status = 0; /* Cycle infinitely, to protect against premature ending */
				fb4_counter = 0;
			}
		}

		/*if ((p.rmsFF_fb[1] - p.rmsFF_fb[0]) * (rmsFF_fb_now - p.rmsFF_fb[1]) < 0.0) {
			rmsFF_fb_now += ff_inc;
		}*/
	}

	//printf("fb4_status = %d\n", fb4_status);
	//fflush(stdout);

	ma_rms_fb = (1 - rmsFF_fb_now) * sqrt(DSPF_dp_vecsum_sq(xin_ptr,size) / (dtype)size) + rmsFF_fb_now * ma_rms_fb;
	return ma_rms_fb;
}

void Audapter::downSampSig(dtype *x, dtype *r, const int nr, const int downfact, const bool bFilt)
// Input parameters:
//	 dsFilt: IIR filter for down-sampling
//	 x: input frame;
//	 r: final output
//	 nr: frameLength after the downsampling
//	 downfact: down-sampling factor
{// filtering and decimation	
	/* filtering */
	if (bFilt)
		downSampFilter.filter(x, downSampBuffer, nr * downfact, 1.0);		

	// decimation
	for(int i0 = 0; i0 < nr; i0++)
	{
		r[i0] = downSampBuffer[downfact *i0];
	}
}


void Audapter::upSampSig(IIR_Filter<dtype> & usFilt, dtype *x, dtype *r, const int nr, const int upfact, const dtype scalefact)
// Input parameters:
//	 usFilt: IIR filter for up-sampling
//	 x: input frame;
//	 r: final output
//	 nr: frameLength after the downsampling
//	 upfact: up-sampling factor
//   scalefact: output scaling factor
{
// interpolation
	for(int i0 = 0; i0 < nr; i0++)
		if (i0 % upfact == 0)
			upSampBuffer[i0] = x[i0 / upfact];
		else
			upSampBuffer[i0] = 0.0;

	// filtering
	usFilt.filter(upSampBuffer, r, nr, upfact * scalefact);
}

dtype Audapter::hz2mel(dtype hz){	// Convert frequency from Hz to mel
	dtype mel;
	mel=1127.01048*log(1+hz/700);
	return mel;
}

dtype Audapter::mel2hz(dtype mel){	// Convert frequency from mel to Hz
	dtype hz;
	hz=(exp(mel/1127.01048)-1)*700;
	return hz;
}

dtype Audapter::locateF2(dtype f2){	
//SC Locate the value of f2 in the pertF2 table, through a binary search.
//SC Usef for subsequent interpolation. 
	dtype loc;
	int k=1<<(pfNBit-1),n;

	for(n=0;n<pfNBit-1;n++){
		if (f2>=p.pertF2[k])
			k=k+(1<<(pfNBit-n-2));
		else
			k=k-(1<<(pfNBit-n-2));
	}	
	if (f2<p.pertF2[k])
		k--;

	loc=(dtype)k;

	loc+=(f2-p.pertF2[k])/(p.pertF2[k+1]-p.pertF2[k]);

	if (loc>=pfNPoints-1){	//pfNPoints=257, so locint not be greater than 255 (pfNPoints-2)
		loc=pfNPoints-1-0.000000000001;
	}
	if (loc<0){
		loc=0;
	}
	return loc;
}

void Audapter::DSPF_dp_cfftr2(int n, dtype * x, dtype * w, int n_min)	//SC Fast Fourier transform on complex numbers
{
	int n2, ie, ia, i, j, k, m;
	dtype rtemp, itemp, c, s;
	n2 = n;

	ie = 1;

	for(k = n; k > n_min; k >>= 1)
	{
		n2 >>= 1;
		ia = 0;
		for(j=0; j < ie; j++)
		{
			for(i=0; i < n2; i++)
			{
				c = w[2*i];
				s = w[2*i+1];
				m = ia + n2;
				rtemp = x[2*ia] - x[2*m];
				x[2*ia] = x[2*ia] + x[2*m];
				itemp = x[2*ia+1] - x[2*m+1];
				x[2*ia+1] = x[2*ia+1] + x[2*m+1];
				x[2*m] = c*rtemp - s*itemp;
				x[2*m+1] = c*itemp + s*rtemp;
				ia++;
			}
			ia += n2;
		}	
		ie <<= 1;
		w = w + k;
	}
}

void Audapter::DSPF_dp_icfftr2(int n, double * x, double * w, int n_min)	//SC Inverse Fast Fourier transform on complex numbers
{
	int n2, ie, ia, i, j, k, m;
	double rtemp, itemp, c, s;
	n2 = n;
	ie = 1;
	for(k = n; k > n_min; k >>= 1)
	{
		n2 >>= 1;
		ia = 0;
		for(j=0; j < ie; j++)
		{
			for(i=0; i < n2; i++)
			{
				c = w[2*i];
				s = w[2*i+1];
				m = ia + n2;
				rtemp = x[2*ia] - x[2*m];
				x[2*ia] = x[2*ia] + x[2*m];
				itemp = x[2*ia+1] - x[2*m+1];
				x[2*ia+1] = x[2*ia+1] + x[2*m+1];
				x[2*m] = c*rtemp + s*itemp;
				x[2*m+1] = c*itemp - s*rtemp;
				ia++;
			}
			ia += n2;
		}
		ie <<= 1;
		w = w + k;
	}
}

void Audapter::bit_rev(double* x, int n)	//SC Bit reversal: an FFT subroutine
{
	int i, j, k;
	double rtemp, itemp;

	j = 0;
	for(i=1; i < (n-1); i++)
	{
		k = n >> 1;
		while(k <= j)
		{
			j -= k;
			k >>= 1;
		}
		j += k;
		if(i < j)
		{
			rtemp = x[j*2];
			x[j*2] = x[i*2];
			x[i*2] = rtemp;
			itemp = x[j*2+1];
			x[j*2+1] = x[i*2+1];
			x[i*2+1] = itemp;
		}
	}
}

void Audapter::smbFft(dtype *fftBuffer, double fftFrame_Size, int sign)
/* 
	
 * FFT routine, (C)1996 S.M.Bernsee. Sign = -1 is FFT, 1 is iFFT (inverse)
	Fills fftBuffer[0...2*fftFrameSize-1] with the Fourier transform of the
	time domain data in fftBuffer[0...2*fftFrameSize-1]. The FFT array takes
	and returns the cosine and sine parts in an interleaved manner, ie.
	fftBuffer[0] = cosPart[0], fftBuffer[1] = sinPart[0], asf. fftFrameSize
	must be a power of 2. It expects a complex input signal (see footnote 2),
	ie. when working with 'common' audio signals our input signal has to be
	passed as {in[0],0.,in[1],0.,in[2],0.,...} asf. In that case, the transform
	of the frequencies of interest is in fftBuffer[0...fftFrameSize].
*/
{
	dtype wr, wi, arg,  *p1, *p2, temp;
	dtype tr, ti, ur, ui, *p1r, *p1i, *p2r, *p2i;
	long i, bitm, j, k, le, le2, logN;
	
	logN = (long)(log(fftFrame_Size)/log(2.)+0.5);


	for (i = 2; i < 2*fftFrame_Size-2; i += 2) 
	{
		for (bitm = 2, j = 0; bitm < 2*fftFrame_Size; bitm <<= 1) 
		{
			if (i & bitm) j++;
			j <<= 1;
		}

		if (i < j) 
		{
			p1 = fftBuffer+i; p2 = fftBuffer+j;
			temp = *p1;
			*(p1++) = *p2;
			*(p2++) = temp; 
			temp = *p1;
			*p1 = *p2; 
			*p2 = temp;
		}
	}

	
	for (k = 0, le = 2; k < logN; k++) 
	{
		le <<= 1;
		le2 = le>>1;
		ur = 1.0;
		ui = 0.0;
		arg = M_PI /(le2>>1);
		wr = cos(arg);
		wi = sign*sin(arg);
		for (j = 0; j < le2; j += 2) 
		{
			p1r = fftBuffer+j; p1i = p1r+1;
			p2r = p1r+le2; p2i = p2r+1;
			for (i = j; i < 2*fftFrame_Size; i += le) 
			{
				tr = *p2r * ur - *p2i * ui;
				ti = *p2r * ui + *p2i * ur;
				*p2r = *p1r - tr; *p2i = *p1i - ti;
				*p1r += tr; *p1i += ti;
				p1r += le; p1i += le;
				p2r += le; p2i += le;
			}
			tr = ur*wr - ui*wi;
			ui = ur*wi + ui*wr;
			ur = tr;
		}


	}

}

//void Audapter::writeSignalsToWavFile(char *wavfn_input, char *wavfn_output) {
void Audapter::writeSignalsToWavFile() {
	char str0[256];
	//char *wavfn;
	char wavfn[256];
	int j0;
	int numSamples = maxRecSize;
	int chunkSize, subChunkSize1 = 16, subChunkSize2;
	short audioFormat = 1;
	int wavSampRate, byteRate;
	short blockAlign;
	signed short wavx;
	//const dtype *algosignal_ptr;
	dtype *algosignal_ptr;
	int i0;
	ofstream wavFileCl;
	fstream wavF;

	const short bitsPerSample = sizeof(signed short) * 8;
	const int bytesPerSample = bitsPerSample / 8;
	const short numChannels = 1;

	//algosignal_ptr = getSignal(size);
	algosignal_ptr = this->signal_recorder[0];

	for (j0 = 0; j0 < 2; j0++) {
		/*if (j0 == 0)
			wavfn = wavfn_input;
		else
			wavfn = wavfn_output;

		printf("wavfn = %s\n", wavfn);*/
		//fflush(stdout);

		if (j0 == 0)			
			sprintf_s(wavfn, sizeof(wavfn), "%sinput_%.3d.wav", wavFileBase, dataFileCnt);
		else			
			sprintf_s(wavfn, sizeof(wavfn), "%soutput_%.3d.wav", wavFileBase, dataFileCnt);
		
		wavFileCl.open(wavfn);
		wavFileCl.close();

		wavF.open(wavfn, ios::binary | ios::out);
		if (!wavF) {
			printf("ERROR: failed to open wav file for writing: %s\n", wavfn);
			exit(1);
		}

		sprintf_s(str0, sizeof(str0), "RIFF");
		wavF.write((char *)str0, sizeof(char) * 4);

		//printf("numSamples = %d\n", numSamples);
		subChunkSize2 = numSamples * numChannels * bytesPerSample;
		//printf("subChunkSize1 = %d\n", subChunkSize1); 
		//printf("subChunkSize2 = %d\n", subChunkSize2);

		chunkSize = 4 + (8 + subChunkSize1) + (8 + subChunkSize2);
		//printf("chunkSize = %d\n", chunkSize);
		wavF.write((char *)(&chunkSize), sizeof(int) * 1);

		sprintf_s(str0, sizeof(str0), "WAVE");
		wavF.write((char *)str0, sizeof(char) * 4);

		sprintf_s(str0, sizeof(str0), "fmt ");
		wavF.write((char *)str0, sizeof(char) * 4);

		wavF.write((char *)(&subChunkSize1), sizeof(int) * 1);
		wavF.write((char *)(&audioFormat), sizeof(short) * 1);
		wavF.write((char *)(&numChannels), sizeof(short) * 1);

		//wavSampRate = getParam((void *)"srate");
		wavSampRate = this->p.sr;
		//printf("wavSampRate = %d\n", wavSampRate);
		wavF.write((char *)(&wavSampRate), sizeof(int) * 1);

		byteRate = wavSampRate * bytesPerSample * numChannels;
		//printf("bytesPerSample = %d\n", bytesPerSample);
		//printf("byteRate = %d\n", byteRate);
		wavF.write((char *)(&byteRate), sizeof(int) * 1);

		blockAlign = bytesPerSample * numChannels;
		wavF.write((char *)(&blockAlign), sizeof(short) * 1);

		wavF.write((char *)(&bitsPerSample), sizeof(short) * 1);

		sprintf_s(str0, "data");
		wavF.write((char *)str0, sizeof(char) * 4);
		wavF.write((char *)(&subChunkSize2), sizeof(int) * 1);

		for (i0 = 0; i0 < maxRecSize; i0++) {
			wavx = (signed short)(32767. * algosignal_ptr[j0 * maxRecSize + i0]);
			//wavx = (signed short)(32767. * 0.99 * sin(2 * M_PI * (dtype)(i0) / wavSampRate * 1000));

			if (wavx > 32767)
				wavx = 32727;
			if (wavx < -32768)
				wavx = -32768;

			wavF.write((char *)(&wavx), sizeof(signed short) * 1);
		}

		wavF.close();
	}

	dataFileCnt++;
}


void Audapter::calcRMSSlope() {
	int i0;
	dtype nom = 0, den = 0;
	dtype mn_x = (rmsSlopeN - 1) / 2;
	dtype mn_y = 0;

	if (data_counter < rmsSlopeN - 1) {
		a_rms_o_slp[data_counter] = 0;
	}
	else {
		// Calculate mean y (mean rms)
		for (i0 = 0; i0 < rmsSlopeN; i0++) {
			mn_y += a_rms_o[data_counter - rmsSlopeN + 1 + i0];
		}
		mn_y /= rmsSlopeN;

		for (i0 = 0; i0 < rmsSlopeN; i0++) {
			den += (i0 - mn_x) * (i0 - mn_x);
			nom += (i0 - mn_x) * (a_rms_o[data_counter - rmsSlopeN + 1 + i0] - mn_y);
		}
	}

	a_rms_o_slp[data_counter] = nom / den / (p.frameLen / (dtype)p.sr);	
}


int Audapter::gainPerturb(dtype *buffer,dtype *gtot_ptr,int framelen, int frameshift)
{// this routine applies the gain factors collected in the vector gTot to the signal
 // for each window (nwin windows in one frame) the function finds the first zerocrossing
 // and updates the gain factor used to scale this window
 // gainadaption is done before deemphasis for two reasons:
	//1: more zerocrossings in the preemphasized signal
	//2: abrupt gain changings are more likely to introduce high frequency noise which will be filtered by the deemphasis filter
 // if no zerocrossing is found in the current window the gain factor will not be updated in this frame
 // assumtpion: zerocrossing is present in nearly each window (can be verified : the value returned by this fuction should equal nWin)
 // gain factor only changes slowely
	int i,index=0,updated=0;
	static dtype gain=1;// 1=no gain adaption =0dB
	static dtype lastSample=buffer[0];// used for continuous zero-crossing finding
	int bGainPert;
	dtype gain_new = intShiftRatio; //SCai(2012/10/19): PIP
	bool armed=true;

	bGainPert = (pertCfg.n > 0);

	if (bGainPert) {
		if (lastSample * buffer[0] <= 0)// zero crossing at first sample ?
		{
			//gain=gtot_ptr[0]; // update gain
			gain = gain_new; // update gain
			armed=false; // zero finding disabled untill next win
			updated++; // number of times gain has been updated (should be equal to nwin)
		}

		for (i=0;i<framelen-1;i++)// scan complete frame 
		{
			
			buffer[i]=gain * buffer[i];// apply gain 

			if (i>=(index+1)*frameshift-1) // next win ?
			{
				index++; // next win
				armed=true; // enable next zero search
			}


			if (armed && (buffer[i] * buffer[i+1] <= 0))// zero crossing in win ? 
			{
				//gain=gtot_ptr[index]; // update gain
				gain = gain_new;
				armed=false; // zero finding disabled untill next win
				updated++; // number of times gain has been updated (should be equal to nwin)
			}

		
		}
		lastSample=buffer[framelen-1]; //store last sample for next function call
		buffer[framelen-1]=gain * buffer[framelen-1];//last sample
	}
	return updated;
}

int Audapter::handleBufferToneSeq(dtype *inFrame_ptr, dtype *outFrame_ptr, int frame_size)	// Tone sequence generator
{
	int n,m;
	double dt=0.00002083333333333333; /* TODO: Make not ad hoc */
	double rt,sineVal,envVal;

	for (n=0;n<frame_size;n++) {
		for (m=p.tsgNTones-1;m>=0;m--) {
			if (p.wgTime>=tsgToneOnsets[m])
				break;
		}
		rt=p.wgTime-tsgToneOnsets[m];
		sineVal=p.tsgToneAmp[m]*sin(2*M_PI*p.tsgToneFreq[m]*rt);
		if (rt>p.tsgToneDur[m]) {
			envVal=0;
		}
		else if (rt<p.tsgToneRamp[m]) {
			envVal=rt/p.tsgToneRamp[m];
		}
		else if (rt>p.tsgToneDur[m]-p.tsgToneRamp[m]) {
			envVal=(p.tsgToneDur[m]-rt)/p.tsgToneRamp[m];
		}
		else {
			envVal=1.;
		}

		outFrame_ptr[2 * n] = outFrame_ptr[2 * n + 1] = sineVal * envVal; /* Stereo */
		tsg_wf[tsgRecCounter++] = outFrame_ptr[2 * n];

		p.wgTime = p.wgTime + dt;
	}

	return 0;
}

void Audapter::readOSTTab(int bVerbose) {
	string str_ostFN(ostFN);

	if ( str_ostFN.size() == 0 ) {
		ostTab.nullify();
		return;
	}
	try {
		ostTab.readFromFile(str_ostFN, bVerbose);
	}
	catch (OST_TAB::ostFileReadingError) {
		std::string errMsgTxt("Fail to read from ost file: ");
		errMsgTxt += str_ostFN;

		mexErrMsgTxt(errMsgTxt.c_str());
	}
	catch (OST_TAB::ostFileSyntaxError err) {
		std::string errMsgTxt("Syntax error in ost file ");
		errMsgTxt += str_ostFN;
		errMsgTxt += " line: \"";
		errMsgTxt += err.errLine;
		errMsgTxt += "\"";
		
		mexErrMsgTxt(errMsgTxt.c_str());
	}
	catch (OST_TAB::unrecognizedOSTModeError err) {
		std::string errMsgTxt("Unrecognized OST heuristic mode: ");
		errMsgTxt += err.modeStr;

		mexErrMsgTxt(errMsgTxt.c_str());
	}
	this->rmsSlopeWin = ostTab.rmsSlopeWin;
}

void Audapter::readPertCfg(int bVerbose) {
	string str_pertCfgFN(pertCfgFN);

	if ( str_pertCfgFN.size() == 0 ) {
		pertCfg.nullify();
		return;
	}

	try {
		pertCfg.readFromFile(str_pertCfgFN, bVerbose);
	}
	catch (PERT_CFG::pcfFileReadingError err) {
		string errMsg("Error reading from pcf file: ");
		errMsg += str_pertCfgFN;

		mexErrMsgTxt(errMsg.c_str());
	}
	catch (PERT_CFG::pcfFileSyntaxError err) {
		string errMsg("Erroneous syntax in pcf file (");
		errMsg += str_pertCfgFN;
		errMsg += ") line: \"";
		errMsg += err.errLine;
		errMsg += "\"";

		mexErrMsgTxt(errMsg.c_str());
	}

	/* Re-initialize phase vocoder according to perturbation config */
	bool bIsPitchShift = false;
	for (int i = 0; i < pertCfg.n; ++i) {
		if (pertCfg.pitchShift[i] != 0.0) {
			bIsPitchShift = true;
			break;
		}
	}

	/* If no pitch shift or time warp is involved,
	   an arbitrary default mode will be used */
	PhaseVocoder::operMode pVocMode = PhaseVocoder::operMode::PITCH_SHIFT_ONLY;
	if (pertCfg.warpCfg.size() > 0 && !bIsPitchShift) {
		pVocMode = PhaseVocoder::TIME_WARP_ONLY;
	}
	else if (pertCfg.warpCfg.size() == 0 && bIsPitchShift) {
		pVocMode = PhaseVocoder::PITCH_SHIFT_ONLY;
	}
	else if (pertCfg.warpCfg.size() > 0 && !bIsPitchShift) {
		pVocMode = PhaseVocoder::TIME_WARP_WITH_FIXED_PITCH_SHIFT;
	}

	if (pVoc)
		delete [] pVoc;

	pVoc = new PhaseVocoder[p.nFB];

	for (int i = 0; i < p.nFB; ++i) {
		try {
			pVoc[i].config(pVocMode, p.nDelay, 
				    	   static_cast<dtype>(p.sr), p.frameLen, 
						   p.pvocFrameLen, p.pvocHop);
		}
		catch (PhaseVocoder::initializationError) {
			mexErrMsgTxt("Failed to initialize phase vocoder");
		}
	}
}


