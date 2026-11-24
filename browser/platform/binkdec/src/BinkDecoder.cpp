/*
 * libbinkdec - Bink video decoder
 * Copyright (C) 2011 Barry Duncan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* This code is based on the Bink decoder from the FFmpeg project which can be obtained from http://www.ffmpeg.org/
 * below is the license from FFmpeg
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Bink video decoder
 * Copyright (c) 2009 Konstantin Shishkov
 * Copyright (C) 2011 Peter Ross <pross@xvid.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "BinkDecoder.h"
#include "LogError.h"
#include "FFmpeg_includes.h"
#include "bd_mem.h"
#include <string.h>

BinkDecoder::BinkDecoder()
{
	nFrames = 0;
	currentFrame = 0;
	for (int i = 0; i < BINK_NB_SRC; i++)
	{
		bundle[i].data = NULL;
	}
}

BinkDecoder::~BinkDecoder()
{
	for (uint32_t i = 0; i < planes.size(); i++)
	{
		bd_free( planes[i].current );
		bd_free( planes[i].last );
	}

	FreeBundles();

	for (uint32_t i = 0; i < audioTracks.size(); i++)
	{
		bd_free( audioTracks[i]->buffer );
		bd_free( audioTracks[i]->blockBuffer );

		if (kTransformTypeRDFT == audioTracks[i]->transformType)
			ff_rdft_end(&audioTracks[i]->trans.rdft);
		else if (kTransformTypeDCT == audioTracks[i]->transformType)
			ff_dct_end(&audioTracks[i]->trans.dct);

		audioTracks[i]->~AudioTrack();
		bd_free( audioTracks[i] );
	}
}

uint32_t BinkDecoder::GetNumFrames() const
{
	return nFrames;
}

uint32_t BinkDecoder::GetCurrentFrameNum() const
{
	return currentFrame;
}

float BinkDecoder::GetFrameRate() const
{
	return (float)fpsDividend / (float)fpsDivider;
}

float BinkDecoder::GetFrameTime() const
{
	return (float)fpsDivider / (float)fpsDividend;
}

void BinkDecoder::GotoFrame(uint32_t frameNum)
{
	// seek to the desired frame (just set currentFrame)
	currentFrame = frameNum;

	// what else? (memset some stuff?)
}

bool BinkDecoder::Open(bdec_file_io_t io, void *usrData)
{
	// open the file (read only)
	file.Open(io, usrData);

	// check the file signature
	signature = file.ReadUint32BE();
	if ((signature != kBIKfID)
		&& (signature != kBIKgID)
		&& (signature != kBIKhID)
		&& (signature != kBIKiID))
	{
		BinkCommon::LogError("Unknown Bink signature");
		return false;
	}

	fileSize = file.ReadUint32LE() + 8;

	nFrames = file.ReadUint32LE();

	if (nFrames > 1000000)
	{
		BinkCommon::LogError("Invalid header, more than 1000000 frames");
		return false;
	}

	largestFrameSize = file.ReadUint32LE();
	if (largestFrameSize > fileSize)
	{
		BinkCommon::LogError("Largest frame size is greater than file size");
		return false;
	}

	// skip some unknown data
	file.Skip(4);

	frameWidth  = file.ReadUint32LE();
	frameHeight = file.ReadUint32LE();
	fpsDividend = file.ReadUint32LE();
	fpsDivider  = file.ReadUint32LE();
	videoFlags  = file.ReadUint32LE();

	nAudioTracks = file.ReadUint32LE();

	// audio is available
	if (nAudioTracks)
	{
		// skip some useless values (unknown and audio channels)
		file.Skip(4 * nAudioTracks);

		for (uint32_t i = 0; i < nAudioTracks; i++)
		{
			uint16_t sampleRate = file.ReadUint16LE();
			uint16_t flags      = file.ReadUint16LE();

			CreateAudioTrack(sampleRate, flags);
		}

		// skip the audio track IDs
		file.Skip(4 * nAudioTracks);
	}

	// read the video frames
	frames.resize(nFrames);

	uint32_t pos, nextPos;

	nextPos = file.ReadUint32LE();

	for (uint32_t i = 0; i < nFrames; i++)
	{
		pos = nextPos;
		if (i == nFrames - 1)
		{
			nextPos = fileSize;
			frames[i].keyFrame = 0;
		}
		else
		{
			nextPos = file.ReadUint32LE();
			frames[i].keyFrame = pos & 1;
		}

		pos &= ~1;
		nextPos &= ~1;

		frames[i].offset = pos;
		frames[i].size   = (nextPos >= pos) ? (nextPos - pos) : 0;
	}

	// NOTE: Do NOT read or skip Frame 0's audio packets here during Open()!
	// SWBF2's RedMoviePlayer supplies a purely sequential BINKIO ring buffer
	// (BinkFileReadHeader/BinkFileReadFrame ignore offsets), so peeking into Frame 0
	// during Open() consumes Frame 0's audio packet before BinkDoFrame(0) runs,
	// desynchronizing every subsequent frame and crashing DecodePlane out-of-bounds
	// on any movie with audioTracks > 0 (such as GOG / localized campaign cutscenes).
	if (nFrames > 0)
	{
		file.Seek(frames[0].offset);
	}

	hasAlpha = videoFlags & kFlagAlpha;
	swapPlanes = signature >= kBIKhID;

	InitBundles();
	InitTrees();

	uint32_t width  = frameWidth;
	uint32_t height = frameHeight;

	// luma plane (BT.601 black Y=16)
	planes.emplace_back().Init(width, height, 16);

	// chroma planes (BT.601 neutral U=128, V=128 — prevents solid green before first decode)
	width  /= 2;
	height /= 2;

	// 1
	planes.emplace_back().Init(width, height, 128);

	// 2
	planes.emplace_back().Init(width, height, 128);

	// alpha plane
	if (hasAlpha)
	{
		width  *= 2;
		height *= 2;

		planes.emplace_back().Init(width, height, 255);
	}

	return true;
}

void BinkDecoder::GetNextFrame(YUVbuffer yuv)
{
	if (currentFrame >= frames.size())
	{
		return;
	}

	// seek to frame offset
	file.Seek(frames[currentFrame].offset);
	uint32_t frameSize = frames[currentFrame].size;

	for (uint32_t trackIndex = 0; trackIndex < audioTracks.size(); trackIndex++)
	{
		// reset bytes read per frame (we might not get any audio for this frame)
		audioTracks[trackIndex]->bytesReadThisFrame = 0;

		if (frameSize < 4)
		{
			frameSize = 0;
			break;
		}

		// check for audio packet size prefix
		uint32_t audioPacketSize = file.ReadUint32LE();
		frameSize -= 4;

		if (audioPacketSize > frameSize)
		{
			// Truncated/malformed audio packet — abort frame safely instead of underflowing frameSize
			frameSize = 0;
			break;
		}

		frameSize -= audioPacketSize;

		// Skip embedded Bink audio packet cleanly (SWBF2 plays cutscene audio via DirectSound/SoundStream,
		// and skipping keeps the file cursor aligned at the exact start of VideoPacket).
		if (audioPacketSize > 0)
		{
			file.Skip(audioPacketSize);
		}
	}

	// get video packet
	if (frameSize > 0)
	{
		VideoPacket(frameSize);
	}

	// set planes data
	for (uint32_t i = 0; i < planes.size(); i++)
	{
		yuv[i].width  = planes[i].width;
		yuv[i].height = planes[i].height;
		yuv[i].pitch  = planes[i].pitch;
		yuv[i].data   = planes[i].last;
	}

	// frame done
	currentFrame++;
}

bool Plane::Init( uint32_t w, uint32_t h, uint8_t fill )
{
	// align to 16 bytes
	w += ( 0x10 - ( w & 0xF ) ) & 0xF;
	h += ( 0x10 - ( h & 0xF ) ) & 0xF;

	const size_t sz = static_cast<size_t>( w ) * h;
	current = static_cast<uint8_t*>( bd_malloc( sz ) );
	last = static_cast<uint8_t*>( bd_malloc( sz ) );
	if (current) memset(current, fill, sz);
	if (last) memset(last, fill, sz);
	this->width = w;
	this->height = h;
	this->pitch = w;

	return true;
}
