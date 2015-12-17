/*
 *    Copyright (C) 2015
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Programming
 *
 *    This file is part of the SDR-J (JSDR).
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#
#include	"dab-constants.h"
#include	"dab-data.h"
#include	"deconvolve.h"
#include	"gui.h"
//
//
//	Interleaving is - for reasons of simplicity - done
//	inline rather than through a special class-object
static
int8_t	interleaveDelays [] = {
	     15, 7, 11, 3, 13, 5, 9, 1, 14, 6, 10, 2, 12, 4, 8, 0};
//
//
//	fragmentsize == Length * CUSize
	dabData::dabData	(RadioInterface *,
	                         int16_t DSCTy,
	                         int16_t packetAddress,
	                         int16_t fragmentSize,
	                         int16_t bitRate,
	                         int16_t uepFlag,
	                         int16_t protLevel,
	                         int16_t FEC_scheme) {
int32_t i, j;
	this	-> DSCTy	= DSCTy;
	this	-> fragmentSize	= fragmentSize;
	this	-> bitRate	= bitRate;
	this	-> uepFlag	= uepFlag;
	this	-> protLevel	= protLevel;
	this	-> FEC_scheme	= FEC_scheme;

	outV			= new uint8_t [2 * bitRate * 24];
	interleaveData		= new int16_t *[fragmentSize]; // the size
	for (i = 0; i < fragmentSize; i ++) {
	   interleaveData [i] = new int16_t [16];
	   for (j = 0; j < 16; j ++)
	      interleaveData [i][j] = 0;
	}
	countforInterleaver	= 0;

	uepProcessor		= NULL;
	eepProcessor		= NULL;
	if (uepFlag == 0)
	   uepProcessor	= new uep_deconvolve (bitRate,
	                                      protLevel);
	else
	   eepProcessor	= new eep_deconvolve (bitRate,
	                                      protLevel);
//
	Buffer		= new RingBuffer<int16_t>(64 * 32768);
	packetState	= 0;
	start ();
}

	dabData::~dabData	(void) {
int16_t	i;
	running = false;
	while (this -> isRunning ())
	   usleep (1);
	delete Buffer;
	if (uepFlag == 0)
	   delete uepProcessor;
	else
	   delete eepProcessor;
	delete[]	outV;
	for (i = 0; i < fragmentSize; i ++)
	   delete[] interleaveData [i];
	delete[]	interleaveData;
}

int32_t	dabData::process	(int16_t *v, int16_t cnt) {
int32_t	fr;
	   while ((fr = Buffer -> GetRingBufferWriteAvailable ()) < cnt) {
	      if (!running)
	         return 0;
	      usleep (10);
	   }

	   Buffer	-> putDataIntoBuffer (v, cnt);
	   Locker. wakeAll ();
	   return fr;
}

void	dabData::run	(void) {
int32_t	countforInterleaver	= 0;
uint8_t	shiftRegister [9];
int16_t	Data [fragmentSize];
int16_t	i, j;

	running	= true;
	while (running) {
	   while (Buffer -> GetRingBufferReadAvailable () < fragmentSize) {
	      ourMutex. lock ();
	      Locker. wait (&ourMutex, 1);	// 1 msec waiting time
	      ourMutex. unlock ();
	      if (!running)
	         break;
	   }

	   if (!running) 
	      break;

	   Buffer	-> getDataFromBuffer (Data, fragmentSize);
//
	   for (i = 0; i < fragmentSize; i ++) {
	      interleaveData [i][interleaveDelays [i & 017]] = Data [i];
	      Data [i] = interleaveData [i] [0];
//	and shift
	      memmove (&interleaveData [i][0],
	               &interleaveData [i][1],
	               interleaveDelays [i & 017] * sizeof (int16_t));
	   }
//
//	only continue when de-interleaver is filled
	   if (countforInterleaver <= 15) {
	      countforInterleaver ++;
	      continue;
	   }
//
	   if (uepFlag == 0)
	      uepProcessor -> deconvolve (Data, fragmentSize, outV);
	   else
	      eepProcessor -> deconvolve (Data, fragmentSize, outV);
//
//	and the inline energy dispersal
	   memset (shiftRegister, 1, 9);
	   for (i = 0; i < bitRate * 24; i ++) {
	      uint8_t b = shiftRegister [8] ^ shiftRegister [4];
	      for (j = 8; j > 0; j--)
	         shiftRegister [j] = shiftRegister [j - 1];
	      shiftRegister [0] = b;
	      outV [i] ^= b;
	   }
//	What we get here is a long sequence of bits, not packed
//	Now we should have a packet
//	we hand it over to make an MSC data group
	
	   handlePacket (outV, 24 * bitRate);
	}
}
//
//	It might take a msec for the task to stop
void	dabData::stopRunning (void) {
	running = false;
	while (this -> isRunning ())
	   usleep (100);
}

static inline
uint16_t	getBits (uint8_t *d, int16_t offset, int16_t size) {
int16_t	i;
uint16_t	res	= 0;

	for (i = 0; i < size; i ++) {
	   res <<= 1;
	   res |= d [offset + i];
	}
	return res;
}

static inline
uint8_t		getBit (uint8_t *d, int16_t offset) {
	return d [offset] != 0 ? 1 : 0;
}
static inline
uint32_t	getLBits	(uint8_t *d,
	                         int16_t offset, int16_t amount) {
uint32_t	res	= 0;
int16_t		i;

	for (i = 0; i < amount; i ++) {
	   res <<= 1;
	   res |= (d [offset + i] & 01);
	}
	return res;
}

void	dabData::handlePacket (uint8_t *data, int16_t length) {
int16_t	packetLength	= (getBits (data, 0, 2) + 1) * 24;
int16_t	continuityIndex	= getBits (data, 2, 2);
int16_t	firstLast	= getBits (data, 4, 2);
int16_t	address		= getBits (data, 6, 10);
uint16_t command	= getBits (data, 16, 1);
int16_t	usefulLength	= getBits (data, 17, 7);
int16_t	i;

	if (!check_mscCRC (data, packetLength * 8))
	   return;

//	assemble the full packet
	if (packetState == 0) {	// waiting for a start
	   if (firstLast == 02) {	// first packet
	      packetState = 1;
	      series. resize (usefulLength * 8);
	      for (i = 0; i < series. size (); i ++)
	         series [i] = data [24 + i];
	   }
	   else
	   if (firstLast == 03) {	// one and only packet
	      series. resize (usefulLength * 8);
	      for (i = 0; i < series. size (); i ++)
	         series [i] = data [24 + i];
	      handleMSCdatagroup (series);
	   }
	   else 
	      series. resize (0);
	}
	else
	if (packetState == 01) {	// within a series
	   if (firstLast == 0) {	// intermediate packet
	      int16_t currentLength = series. size ();
	      series. resize (currentLength + 8 * usefulLength);
	      for (i = 0; i < 8 * usefulLength; i ++)
	         series [currentLength + i] = data [24 + i];
	   }
	   else
	   if (firstLast == 01) {	// last packet
	      int16_t currentLength = series. size ();
	      series. resize (currentLength + 8 * usefulLength);
	      for (i = 0; i < 8 * usefulLength; i ++)
	         series [currentLength + i] = data [24 + i];
	      handleMSCdatagroup (series);
	      packetState = 0;
	   }
	   else {
	      packetState = 0;
	      series. resize (0);
	   }
	}
}

void	dabData::handleMSCdatagroup (QByteArray msc) {
uint8_t *data		= (uint8_t *)(msc. data ());
bool	extensionFlag	= getBits (data, 0, 1) != 0;
bool	crcFlag		= getBits (data, 1, 1) != 0;
bool	segmentFlag	= getBits (data, 2, 1) != 0;
bool	userAccessFlag	= getBits (data, 3, 1) != 0;
uint8_t	groupType	= getBits (data, 4, 4);
uint8_t	CI		= getBits (data, 8, 4);
int16_t	next		= 16;
uint16_t segmentNumber	= 0;
bool transportIdFlag	= false;
uint16_t transportId	= 0;
uint8_t	lengthInd;

//	if (crcFlag && !check_mscCRC (data, msc.size ()))
//	   return;
//
	if (extensionFlag)
	   next += 16;

	if (segmentFlag) {
	   segmentNumber = getLBits (data, next + 1, 15);
	   next += 16;
	}

	if (userAccessFlag) {
	   transportIdFlag	= getBits (data, next + 3, 1);
	   lengthInd		= getBits (data, next + 4, 4);
	   next	+= 8;
	   if (transportIdFlag) {
	      transportId = getLBits (data, next, 16);
	      next	+= 16;
	   }
	   next	+= (lengthInd - 2) * 8;
	}
	switch (DSCTy) {
	   case 5:		// TPEG channel
	      fprintf (stderr, "mode 5, groupType = %d\n", groupType);
	      break;
	   case 59:		// embedded IP
	      break;
	   case 60:		// MOT
	      processMSCdatagroup (&data [next], 
	                           msc. size () - next - (crcFlag != 0 ? 16 : 0),
	                           groupType,
	                           segmentNumber,
	                           transportIdFlag,
	                           transportId);
	}
}

void	dabData::processMSCdatagroup (uint8_t	*data,
	                              int16_t	length,
	                              uint8_t	groupType,
	                              int16_t	segmentNumber,
	                              bool	transportIdFlag,
	                              uint16_t	transportId) {
uint16_t segmentSize;
	if (!transportIdFlag)
	   return;		// sorry
	segmentSize	= getBits (data, 3, 13);
uint8_t repetitionCount	= getBits (data, 0, 3);

	if ((segmentNumber == 0) && (groupType == 3)) { // header
	   uint32_t bodySize = getLBits (data, 16, 28);
	   uint32_t headerSize	= getBits (data, 16 + 28, 13);
	   uint8_t  contentType	= getBits (data, 16 + 41,  6);
	   uint16_t subType	= getBits (data, 16 + 47,  9);
	   fprintf (stderr, "new MOT %d (sizes %d %d), segment %d, content %d (%d)\n", 
	         transportId, bodySize, headerSize,
	         segmentNumber, contentType, subType);
	}
	else
	if (groupType == 4) {
	   fprintf (stderr, "Ti = %d, segmentNumber %d, segmentSize %d\n",
	            transportId, segmentNumber, segmentSize);
	}
	else
	   fprintf (stderr, "grouptype = %d, Ti = %d, sn = %d, ss = %d\n",
	                     groupType, transportId, segmentNumber, segmentSize);
}

static
const uint8_t crcPolynome [] =
	{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};	// MSB .. LSB

bool	dabData::check_mscCRC (uint8_t *in, int16_t size) {
int16_t	i, f;
uint8_t	b [16];
int16_t	Sum	= 0;

	memset (b, 1, 16);

	for (i = size - 16; i < size; i ++)
	   in [i] ^= 1;

	for (i = 0; i < size; i++) {
	   if ((b [0] ^ in [i]) == 1) {
	      for (f = 0; f < 15; f++) 
	         b [f] = crcPolynome [f] ^ b[f + 1];
	      b [15] = 1;
	   }
	   else {
	      memmove (&b [0], &b[1], sizeof (uint8_t ) * 15); // Shift
	      b [15] = 0;
	   }
	}

	for (i = 0; i < 16; i++)
	   Sum += b [i];

	return Sum == 0;
}

