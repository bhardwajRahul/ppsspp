//////////////////////////////////////////////////////////////////////////
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////



#pragma once

#include "CaptureDevice.h"
#include <wrl/client.h>


//-------------------------------------------------------------------
//  VideoBufferLock class
//
//  Locks a video buffer that might or might not support IMF2DBuffer.
//
//-------------------------------------------------------------------

class VideoBufferLock
{
public:
	VideoBufferLock(IMFMediaBuffer *pBuffer) : m_p2DBuffer(NULL), m_bLocked(FALSE)
	{
		m_pBuffer = pBuffer;
	
		// Query for the 2-D buffer interface. OK if this fails.
		(void)m_pBuffer->QueryInterface(IID_PPV_ARGS(&m_p2DBuffer));
	}

	~VideoBufferLock()
	{
		UnlockBuffer();
	}

	//-------------------------------------------------------------------
	// LockBuffer
	//
	// Locks the buffer. Returns a pointer to scan line 0 and returns the stride.
	//
	// The caller must provide the default stride as an input parameter, in case
	// the buffer does not expose IMF2DBuffer. You can calculate the default stride
	// from the media type.
	//-------------------------------------------------------------------

	HRESULT LockBuffer(
		LONG  lDefaultStride,    // Minimum stride (with no padding).
		DWORD dwHeightInPixels,  // Height of the image, in pixels.
		BYTE  **ppbScanLine0,    // Receives a pointer to the start of scan line 0.
		LONG  *plStride          // Receives the actual stride.
	)
	{
		HRESULT hr = S_OK;

		// Use the 2-D version if available.
		if (m_p2DBuffer)
		{
			hr = m_p2DBuffer->Lock2D(ppbScanLine0, plStride);
		}
		else
		{
			// Use non-2D version.
			BYTE *pData = NULL;

			hr = m_pBuffer->Lock(&pData, NULL, NULL);
			if (SUCCEEDED(hr))
			{
				*plStride = lDefaultStride;
				if (lDefaultStride < 0)
				{
					// Bottom-up orientation. Return a pointer to the start of the
					// last row *in memory* which is the top row of the image.
					*ppbScanLine0 = pData + abs(lDefaultStride) * (dwHeightInPixels - 1);
				}
				else
				{
					// Top-down orientation. Return a pointer to the start of the
					// buffer.
					*ppbScanLine0 = pData;
				}
			}
		}

		m_bLocked = (SUCCEEDED(hr));

		return hr;
	}

	//-------------------------------------------------------------------
	// UnlockBuffer
	//
	// Unlocks the buffer. Called automatically by the destructor.
	//-------------------------------------------------------------------

	void UnlockBuffer()
	{
		if (m_bLocked)
		{
			if (m_p2DBuffer)
			{
				(void)m_p2DBuffer->Unlock2D();
			}
			else
			{
				(void)m_pBuffer->Unlock();
			}
			m_bLocked = FALSE;
		}
	}

private:
	Microsoft::WRL::ComPtr<IMFMediaBuffer>  m_pBuffer;
	Microsoft::WRL::ComPtr<IMF2DBuffer>     m_p2DBuffer;

	BOOL            m_bLocked;
};
