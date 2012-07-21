﻿//(XviD4PSP5) modded version
/*
 * Copyright (c) 2004-2008 Mike Matsnev.  All Rights Reserved.
 * 
 * $Id: avss.cpp,v 1.7 2008/03/29 15:41:28 mike Exp $
 * 
 */

#define _ATL_FREE_THREADED
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS	// some CString constructors will be explicit
#define _ATL_ALL_WARNINGS
#define ERRMSG_LEN 1024

#include <windows.h>
#include <tchar.h>
#include <dshow.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlstr.h>
#include <atlcoll.h>
#include "VideoSink.h"
#include "utils.h"
#include "guids.h"

#if defined AVS_26
#include "avisynth_26.h"
#else
#include "avisynth.h"
#endif

class DSS2 : public IClip
{
	struct DF
	{
		REFERENCE_TIME  timestamp;  // DS timestamp that we used for this frame
		PVideoFrame     frame;      // cached AVS frame

		DF() : timestamp(-1) { }
		DF(PVideoFrame f) : timestamp(-1), frame(f) { }
		DF(const DF& f) { operator=(f); }
		DF& operator=(const DF& f) { timestamp = f.timestamp; frame = f.frame; return *this; }
	};

	CAtlArray<DF>           m_f;
	int                     m_qfirst,
		                    m_qnext,
		                    m_qmax,
		                    m_seek_thr,
		                    m_preroll,
		                    m_do_not_trespass;
	PVideoFrame             m_last_good_frame;

	CComPtr<IVideoSink>     m_pR;
	CComPtr<IMediaControl>  m_pGC;
	CComPtr<IMediaSeeking>  m_pGS;
	HANDLE                  m_hFrameReady;
	bool                    m_registered;
	DWORD                   m_rot_cookie;

	VideoInfo               m_vi;
	__int64                 m_avgframe;

	void RegROT()
	{
		if (!m_pGC || m_registered)
			return;

		CComPtr<IRunningObjectTable> rot;
		if (FAILED(GetRunningObjectTable(0, &rot)))
			return;

		CStringA name;
		name.Format("FilterGraph %08p pid %08x (avss)", m_pGC.p, GetCurrentProcessId());

		CComPtr<IMoniker> mk;
		if (FAILED(CreateItemMoniker(L"!", CA2W(name), &mk)))
			return;

		if (SUCCEEDED(rot->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, m_pGC, mk, &m_rot_cookie)))
			m_registered = true;
	}

	void UnregROT()
	{
		if (!m_registered)
			return;

		CComPtr<IRunningObjectTable> rot;
		if (FAILED(GetRunningObjectTable(0, &rot)))
			return;

		if (SUCCEEDED(rot->Revoke(m_rot_cookie)))
			m_registered = false;
	}

	HRESULT Open(const wchar_t *filename, char *err, __int64 avgf)
	{
		HRESULT hr;

		CComPtr<IGraphBuilder> pGB; //CLSID_FilterGraphNoThread
		if (FAILED(hr = pGB.CoCreateInstance(CLSID_FilterGraph))) {
			SetError("Create FilterGraph: ", err); return hr; }

		CComPtr<IBaseFilter> pVS;
		if (FAILED(hr = CreateVideoSink(&pVS))) {
			SetError("Create VideoSink: ", err); return hr; }

		if (FAILED(hr = pGB->AddFilter(pVS, L"VideoSink"))) {
			SetError("Add VideoSink: ", err); return hr; }

		CComQIPtr<IVideoSink> sink(pVS);
		if (!sink) { SetError("Get IVideoSink: ", err); return E_NOINTERFACE; }

		CComQIPtr<IVideoSink2> sink2(pVS);
		if (!sink2) { SetError("Get IVideoSink2: ", err); return E_NOINTERFACE; }

		sink->SetAllowedTypes(IVS_RGB32|IVS_YV12|IVS_YUY2);
		ResetEvent(m_hFrameReady);
		sink2->NotifyFrame(m_hFrameReady);

		CComPtr<IBaseFilter> pSrc;
		if (FAILED(hr = pGB->AddSourceFilter(filename, NULL, &pSrc))) {
			SetError("Add Source: ", err); return hr; }

		//Это отключает иконку Haali-сплиттера в трее (зачем?!)
		//CComQIPtr<IPropertyBag> pPB(pSrc);
		//if (pPB) pPB->Write(L"ui.interactive", &CComVariant(0u, VT_UI4));

		CComPtr<IPin> pSrcOutP(GetPin(pSrc, false, PINDIR_OUTPUT, &MEDIATYPE_Video));
		if (!pSrcOutP) pSrcOutP = GetPin(pSrc, false, PINDIR_OUTPUT, &MEDIATYPE_Stream);

		CComPtr<IPin> pVSInP(GetPin(pVS, false, PINDIR_INPUT));

		if (!pSrcOutP || !pVSInP) {
			SetError("GetPin: ", err); return E_FAIL; }

		if (FAILED(hr = pGB->Connect(pSrcOutP, pVSInP))) {
			SetError("Connect filters: ", err); return hr; }

		CComQIPtr<IMediaControl> mc(pGB);
		if (!mc) { SetError("Get IMediaControl: ", err); return E_NOINTERFACE; }

		CComQIPtr<IMediaSeeking> ms(pGB);
		if (!ms) { SetError("Get IMediaSeeking: ", err); return E_NOINTERFACE; }

		if (FAILED(hr = mc->Run())) {
			SetError("Run FilterGraph: ", err); return hr; }

		OAFilterState fs;
		if (FAILED(hr = mc->GetState(2000, &fs))) {
			SetError("GetState: ", err); return hr; }

		// wait for the first frame to arrive (up to 10s) //5s
		if (WaitForSingleObject(m_hFrameReady, 10000) != WAIT_OBJECT_0) {
			SetError("Timeout waiting for FrameReady: ", err); return E_FAIL; }

		__int64 defd;
		unsigned  type, width, height, arx, ary;
		if (FAILED(hr = sink2->GetFrameFormat(&type, &width, &height, &arx, &ary, &defd))) {
			SetError("GetFrameFormat: ", err); return hr; }

		REFERENCE_TIME duration;
		if (FAILED(hr = ms->GetDuration(&duration))) {
			SetError("GetDuration: ", err); return hr; }

		if (defd <= 0)
			defd = 400000;

		if (avgf > 0)
			defd = avgf;

		switch (type)
		{
			case IVS_RGB32: m_vi.pixel_type = VideoInfo::CS_BGR32; break;
			case IVS_YUY2: m_vi.pixel_type = VideoInfo::CS_YUY2; break;
			case IVS_YV12: m_vi.pixel_type = VideoInfo::CS_YV12; break;
			default: { SetError("Unsupported colorspace: ", err); return E_FAIL; }
		}

		m_vi.width = width;
		m_vi.height = height;

		m_vi.num_frames = (int)(duration / defd);
		m_vi.SetFPS(10000000, (unsigned int)defd);

		m_avgframe = defd;

		m_pR = sink;
		m_pGC = mc;
		m_pGS = ms;

		SetEvent(m_hFrameReady);

		RegROT();

		m_f.SetCount(m_vi.num_frames);

		return S_OK;
	}

	void  Close()
	{
		CComQIPtr<IVideoSink2> pVS2(m_pR);
		if (pVS2) pVS2->NotifyFrame(NULL);

		m_f.RemoveAll();

		UnregROT();

		m_pR.Release();
		m_pGC.Release();
		m_pGS.Release();
	}

	struct RFArg
	{
		DF        *df;
		VideoInfo *vi;
	};

	static void ReadFrame(__int64 timestamp, unsigned format, unsigned bpp, const unsigned char *frame, unsigned width, unsigned height, int stride, unsigned arx, unsigned ary, void *arg)
	{
		RFArg     *rfa = (RFArg *)arg;
		DF        *df = rfa->df;
		VideoInfo *vi = rfa->vi;

		df->timestamp = timestamp;

		int w_cp = min((int)width, vi->width);
		int h_cp = min((int)height, vi->height);

		if ((format == IVS_RGB32 && vi->pixel_type == VideoInfo::CS_BGR32) ||
			(format == IVS_YUY2 && vi->pixel_type == VideoInfo::CS_YUY2))
		{
			BYTE *dst = df->frame->GetWritePtr();
			int  dstride = df->frame->GetPitch();

			w_cp *= bpp;

			for (int y = 0; y < h_cp; ++y)
			{
				memcpy(dst, frame, w_cp);
				frame += stride;
				dst += dstride;
			}
		}
		else if (format == IVS_YV12 && vi->pixel_type == VideoInfo::CS_YV12)
		{
			// plane Y
			BYTE                *dp = df->frame->GetWritePtr(PLANAR_Y);
			const unsigned char *sp = frame;
			int                 dstride = df->frame->GetPitch(PLANAR_Y);

			for (int y = 0; y < h_cp; ++y)
			{
				memcpy(dp, sp, w_cp);
				sp += stride;
				dp += dstride;
			}

			// UV
			dstride >>= 1;
			stride >>= 1;
			w_cp >>= 1;
			h_cp >>= 1;

			// plane V
			dp = df->frame->GetWritePtr(PLANAR_V);
			sp = frame + height * stride * 2;
			dstride = df->frame->GetPitch(PLANAR_V);

			for (int y = 0; y < h_cp; ++y)
			{
				memcpy(dp, sp, w_cp);
				sp += stride;
				dp += dstride;
			}

			// plane U
			dp = df->frame->GetWritePtr(PLANAR_U);
			sp = frame + height * stride * 2 + (height >> 1) * stride;
			dstride = df->frame->GetPitch(PLANAR_U);

			for (int y = 0; y < h_cp; ++y)
			{
				memcpy(dp, sp, w_cp);
				sp += stride;
				dp += dstride;
			}
		}
	}

	bool NextFrame(IScriptEnvironment *env, DF& rdf, int& fn)
	{
		while (true)
		{
			//Ждем не дольше 10-ти сек., т.к. ждать INFINITE - слишком долго :)
			if (WaitForSingleObject(m_hFrameReady, 10000) != WAIT_OBJECT_0)
			{
				//Запоминаем номер кадра, получая который мы зависли
				m_do_not_trespass = fn;

				m_pR->Reset();
				return false;
			}

			DF df(env->NewVideoFrame(m_vi));
			RFArg arg = { &df, &m_vi };

			HRESULT hr = m_pR->ReadFrame(ReadFrame, &arg);
			if (FAILED(hr))
				return false;

			if (hr == S_FALSE) // EOF
			{
				//Запоминаем номер кадра, получая который мы дошли до EOF
				m_do_not_trespass = fn;

				m_pR->Reset();
				return false;
			}

			//Картинка последнего успешно декодированного кадра
			m_last_good_frame = df.frame;

			if (df.timestamp >= 0)
			{
				int frameno = (int)((double)df.timestamp / m_avgframe + 0.5);
				if (frameno >= 0 && frameno < (int)m_f.GetCount())
				{
					fn = frameno;
					rdf = df;
					return true;
				}
			}
		}
	}

public:
	DSS2() :
		m_registered(false),
		m_qfirst(0),                //Номер кадра, с которого начинается кэш.
		m_qnext(0),                 //Номер текущего кадра + 1 (т.е. следующий кадр).
		m_qmax(10),                 //Макс. кол-во кадров, хранимых в кэше, а так-же размер кэшируемого "прыжка назад" при последовательном чтении в обратную сторону.
		m_seek_thr(100),            //Порог для сикинга вперёд. Если требуемый кадр дальше от текущего на эту величину - будет сикинг, иначе будем ползти покадрово.
		m_preroll(0),               //На сколько кадров "недосикивать" (т.е. отступ при сикинге), оставшееся будет пройдено покадрово - помогает выдать нужный кадр на "плохосикающихся" файлах.
		m_do_not_trespass(INT_MAX)  //Номер кадра, дальше которого двигаться нельзя (конец файла\потока).
	{                               //P.S. Актуальные дефолты перенесены отсюда в Create_DSS2()
		m_hFrameReady = CreateEvent(NULL, FALSE, FALSE, NULL);
		memset(&m_vi, 0, sizeof(m_vi));
	}

	~DSS2()
	{
		Close();
		CloseHandle(m_hFrameReady);
	}

	HRESULT OpenFile(const wchar_t *filename, char *err, __int64 avgframe, int cache, int seekthr, int preroll)
	{
		m_qmax = cache;
		m_seek_thr = seekthr;
		m_preroll = preroll;

		return Open(filename, err, avgframe);
	}

	// IClip
	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env)
	{
		if (n < 0 || n >= (int)m_f.GetCount())
			env->ThrowError("DSS2: GetFrame: Requested frame number is out of range!");

		if (m_f[n].frame)
			return m_f[n].frame;

		// we need to seek to get this frame
		if (n < m_qfirst || n > m_qnext + m_seek_thr)
		{
			// clear buffered frames
			for (int i = m_qfirst; i < m_qnext; ++i)
				m_f[i].frame = NULL;

			// reset read postion
			if (n >= m_qfirst - m_qmax && n < m_qfirst) // assume we are scanning backwards (не дальше, чем на 10(m_qmax) кадров назад от начала кэша)
			{
				m_qfirst = n - m_qmax + 1;       //Отступ на -9 (m_qmax-1) кадров от требуемого, "лишние" кадры (от m_qfirst до n) будут кэшированы.
				if (m_qfirst < 0) m_qfirst = 0;  //Т.е. чтение назад идет не покадрово, а "прыжками" по 9-ть кадров, по мере надобности кадры выдаются из кэша.
				m_qnext = m_qfirst;
			}
			else
			{
				// just some random seek
				m_qfirst = m_qnext = n;
			}

			// seek the graph
			ResetEvent(m_hFrameReady);

			//Отступ (без кэширования "лишних" кадров)
			int pos_frame = m_qfirst - m_preroll;
			if (pos_frame < 0) pos_frame = 0;

			REFERENCE_TIME pos_time = m_avgframe * pos_frame - 10001; // -1ms, account for typical timestamps rounding
			if (pos_time < 0) pos_time = 0;

			if (FAILED(m_pGS->SetPositions(&pos_time, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning)))
				env->ThrowError("DSS2: DirectShow seeking failed!");

			//Сброс защиты от "последнего кадра"
			m_do_not_trespass = INT_MAX;

			//Сброс последнего полученного изображения
			m_last_good_frame = NULL;
		}

		// we performed a seek or are reading sequentially
		while (true)
		{
			DF df;
			int frameno = n;

			//Если дальше нельзя или просто некуда - выдаем последний декодированный кадр
			if (n >= m_do_not_trespass || !NextFrame(env, df, frameno))
			{
				//Обновляем m_qnext, иначе через m_seek_thr кадров будет попытка сделать сикинг.
				//Это нужно для совсем кривых файлов, у которых duration определяется намного больше, чем есть на самом деле.
				if (m_qnext < (int)m_f.GetCount() - 1)
					m_qnext += 1;

				if (!m_last_good_frame)
				{
					//Если выдать нечего - создаем "серый" кадр (из DSS)
					m_last_good_frame = env->NewVideoFrame(m_vi);
					memset(m_last_good_frame->GetWritePtr(), 128, m_last_good_frame->GetPitch() * m_last_good_frame->GetHeight() +
						m_last_good_frame->GetPitch(PLANAR_U) * m_last_good_frame->GetHeight(PLANAR_U) * 2);
				}
				return m_last_good_frame;
			}

			// let's see what we've got
			if (frameno < m_qnext)
			{
				// preroll frame
				continue;
			}
			else if (frameno == m_qnext)
			{ 
				// we want this frame, compare timestamps to account for decimation
				if (m_f[frameno].timestamp < 0) // we see this for the first time
					m_f[frameno].timestamp = df.timestamp;

				if (df.timestamp < m_f[frameno].timestamp) // early, ignore
					continue;

				// this is the frame we want
				m_f[frameno].frame = df.frame;
			}
			else //frameno > m_qnext
			{ 
				// somehow we got beyond the place we wanted, fill in intermediate frames
				// don't check for timestamps, just use what we've got
				for (int i = m_qnext; i <= frameno; ++i)
				{
					if (m_f[i].timestamp < 0)
						m_f[i].timestamp = df.timestamp;
					m_f[i].frame = df.frame;
				}
			}

			// keep cached frames below max
			for (; m_qnext - m_qfirst > m_qmax && m_qfirst < m_qnext && m_qfirst < n; ++m_qfirst)
				m_f[m_qfirst].frame = NULL;

			m_qnext = frameno + 1;

			if (n >= m_qfirst && n < m_qnext)
				return m_f[n].frame;
		}
	}

	const VideoInfo& __stdcall GetVideoInfo() { return m_vi; }

	// TODO
	void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env) { memset(buf, 0, (size_t)m_vi.BytesFromAudioSamples(count)); }
	bool __stdcall GetParity(int n) { return true; }
#ifdef AVS_26
	int __stdcall SetCacheHints(int cachehints, int frame_range) { return 0; }
#else
	void __stdcall SetCacheHints(int cachehints, int frame_range) { }
#endif

	void SetError(const char *text, char *err)
	{
		if (strlen(err) > 0)
		{
			//Append
			char err_temp[ERRMSG_LEN] = {0};
			strncpy_s(err_temp, ERRMSG_LEN, err, _TRUNCATE);
			strncpy_s(err, ERRMSG_LEN, text, _TRUNCATE);
			strncat_s(err, ERRMSG_LEN, err_temp, _TRUNCATE);
		}
		else
		{
			//Copy
			strncpy_s(err, ERRMSG_LEN, text, _TRUNCATE);
		}
	}
};

static AVSValue __cdecl Create_DSS2(AVSValue args, void*, IScriptEnvironment* env)
{
	if (args[0].ArraySize() != 1)
		env->ThrowError("DSS2: Only 1 filename currently supported!");

	const char *filename = args[0][0].AsString();
	if (filename == NULL || strlen(filename) == 0)
		env->ThrowError("DSS2: Filename expected!");

	__int64 avgframe = args[1].Defined() ? (__int64)(10000000ll / args[1].AsFloat() + 0.5) : 0; //fps       (>0, undef.)
	int cache = __max(args[2].AsInt(10), 1);                                                    //cache     (>=1, def=10)
	int seekthr = __max(args[3].AsInt(100), 1);                                                 //seekthr   (>=1, def=100)
	int preroll = __max(args[4].AsInt(0), 0);                                                   //preroll   (>=0, def=0)

	DSS2 *dss2 = new DSS2();
	char err[ERRMSG_LEN] = {0};

	HRESULT hr = dss2->OpenFile(CA2WEX<128>(filename, CP_ACP), err, avgframe, cache, seekthr, preroll);
	if (FAILED(hr))
	{
		delete dss2;

		//Расшифровка HRESULT и формирование сообщения об ошибке (из DSS)
		char hr_err[MAX_ERROR_TEXT_LEN] = {0}; //string[1024]
		if (AMGetErrorTextA(hr, hr_err, MAX_ERROR_TEXT_LEN) > 0) //1023
			env->ThrowError("DSS2: Can't open \"%s\"\n\n%s(%08x) %s\n", filename, err, hr, hr_err);
		env->ThrowError("DSS2: Can't open \"%s\"\n\n%s(%08x) Unknown error.\n", filename, err, hr);
	}

	return dss2;
}

#ifdef AVS_26
const AVS_Linkage *AVS_linkage = 0;
extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors)
{
	AVS_linkage = vectors;
	env->AddFunction("DSS2", "s+[fps]f[cache]i[seekthr]i[preroll]i", Create_DSS2, 0);
	return "DSS2";
}
#else
extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env)
{
	env->AddFunction("DSS2", "s+[fps]f[cache]i[seekthr]i[preroll]i", Create_DSS2, 0);
	return "DSS2";
}
#endif
