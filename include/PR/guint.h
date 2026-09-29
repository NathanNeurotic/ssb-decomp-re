/**************************************************************************
 *									  *
 *		 Copyright (C) 1994, Silicon Graphics, Inc.		  *
 *									  *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may  not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *									  *
 **************************************************************************/

#include <PR/mbi.h>
#include <PR/gu.h>

typedef union
{
	struct
	{
#ifdef PLATFORM_PS2 // little-endian: the high word of a double is the second one
		unsigned int lo;
		unsigned int hi;
#else
		unsigned int hi;
		unsigned int lo;
#endif
	} word;

	double d;
} du;

/* Initialiser for a du constant from its high and low words. */
#ifdef PLATFORM_PS2
#define DU(hi, lo) {{ (lo), (hi) }}
#else
#define DU(hi, lo) {{ (hi), (lo) }}
#endif

typedef union
{
	unsigned int i;
	float f;
} fu;

#ifndef __GL_GL_H__

typedef float Matrix[4][4];

#endif

#define ROUND(d) (int)(((d) >= 0.0) ? ((d) + 0.5) : ((d)-0.5))
#define ABS(d) ((d) > 0) ? (d) : -(d)

extern float __libm_qnan_f;
