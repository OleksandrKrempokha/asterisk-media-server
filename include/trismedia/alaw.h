/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief A-Law to Signed linear conversion
 */

#ifndef _TRISMEDIA_ALAW_H
#define _TRISMEDIA_ALAW_H


/*! \brief
 * To init the alaw to slinear conversion stuff, this needs to be run.
 */
void tris_alaw_init(void);

#define TRIS_ALAW_BIT_LOSS  4
#define TRIS_ALAW_STEP      (1 << TRIS_ALAW_BIT_LOSS)
#define TRIS_ALAW_TAB_SIZE  (32768 / TRIS_ALAW_STEP + 1)
#define TRIS_ALAW_SIGN_BIT  0x80
#define TRIS_ALAW_AMI_MASK  0x55


/*! \brief converts signed linear to alaw */
#ifndef G711_NEW_ALGORITHM
extern unsigned char __tris_lin2a[8192];
#else
extern unsigned char __tris_lin2a[TRIS_ALAW_TAB_SIZE];
#endif

/*! help */
extern short __tris_alaw[256];

#ifndef G711_NEW_ALGORITHM
#define TRIS_LIN2A(a) (__tris_lin2a[((unsigned short)(a)) >> 3])
#else
#define TRIS_LIN2A_LOOKUP(mag)							\
	__tris_lin2a[(mag) >> TRIS_ALAW_BIT_LOSS]

/*! \brief Convert signed linear sample to sign-magnitude pair for a-Law */
static inline void tris_alaw_get_sign_mag(short sample, unsigned *sign, unsigned *mag)
{
	/* It may look illogical to retrive the sign this way in both cases,
	 * but this helps gcc eliminate the branch below and produces
	 * faster code */
	*sign = ((unsigned short)sample >> 8) & TRIS_ALAW_SIGN_BIT;
#if defined(G711_REDUCED_BRANCHING)
	{
		unsigned dual_mag = (-sample << 16) | (unsigned short)sample;
		*mag = (dual_mag >> (*sign >> 3)) & 0xffffU;
	}
#else
	if (sample < 0)
		*mag = -sample;
	else
		*mag = sample;
#endif /* G711_REDUCED_BRANCHING */
	*sign ^= TRIS_ALAW_SIGN_BIT;
}

static inline unsigned char TRIS_LIN2A(short sample)
{
	unsigned mag, sign;
	tris_alaw_get_sign_mag(sample, &sign, &mag);
	return (sign | TRIS_LIN2A_LOOKUP(mag)) ^ TRIS_ALAW_AMI_MASK;
}
#endif

#define TRIS_ALAW(a) (__tris_alaw[(int)(a)])

#endif /* _TRISMEDIA_ALAW_H */
