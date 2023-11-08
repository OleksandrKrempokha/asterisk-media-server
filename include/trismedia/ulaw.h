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
 * \brief u-Law to Signed linear conversion
 */

#ifndef _TRISMEDIA_ULAW_H
#define _TRISMEDIA_ULAW_H


/*!
 * To init the ulaw to slinear conversion stuff, this needs to be run.
 */
void tris_ulaw_init(void);

#define TRIS_ULAW_BIT_LOSS  3
#define TRIS_ULAW_STEP      (1 << TRIS_ULAW_BIT_LOSS)
#define TRIS_ULAW_TAB_SIZE  (32768 / TRIS_ULAW_STEP + 1)
#define TRIS_ULAW_SIGN_BIT  0x80

/*! \brief converts signed linear to mulaw */
#ifndef G711_NEW_ALGORITHM
extern unsigned char __tris_lin2mu[16384];
#else
extern unsigned char __tris_lin2mu[TRIS_ULAW_TAB_SIZE];
#endif

/*! help */
extern short __tris_mulaw[256];

#ifndef G711_NEW_ALGORITHM

#define TRIS_LIN2MU(a) (__tris_lin2mu[((unsigned short)(a)) >> 2])

#else

#define TRIS_LIN2MU_LOOKUP(mag)											\
	__tris_lin2mu[((mag) + TRIS_ULAW_STEP / 2) >> TRIS_ULAW_BIT_LOSS]


/*! \brief convert signed linear sample to sign-magnitude pair for u-Law */
static inline void tris_ulaw_get_sign_mag(short sample, unsigned *sign, unsigned *mag)
{
       /* It may look illogical to retrive the sign this way in both cases,
        * but this helps gcc eliminate the branch below and produces
        * faster code */
       *sign = ((unsigned short)sample >> 8) & TRIS_ULAW_SIGN_BIT;
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
}

static inline unsigned char TRIS_LIN2MU(short sample)
{
       unsigned mag, sign;
       tris_ulaw_get_sign_mag(sample, &sign, &mag);
       return ~(sign | TRIS_LIN2MU_LOOKUP(mag));
}
#endif

#define TRIS_MULAW(a) (__tris_mulaw[(a)])

#endif /* _TRISMEDIA_ULAW_H */
