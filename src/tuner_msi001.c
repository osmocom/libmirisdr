/*
 * Mirics MSi001 tuner driver
 *
 * Copyright (C) 2012 by Eric Wild <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tuner_msi001.h"
#include "mirisdr_reg.h"

#include <stdint.h>
#include <stdio.h>
#include <memory.h>

static const struct r0_modes_ r0_modes[] = {
	{"01100001", 0x61, "AM_MODE1", 16},
	{"11100001", 0xe1, "AM_MODE2", 16},
	{"01000010", 0x42, "VHF_MODE", 32},
	{"01000100", 0x44, "B3_MODE", 16},
	{"01001000", 0x48, "B45_MODE", 4},
	{"01010000", 0x50, "BL_MODE", 2}
};

static const struct iffreqs_ iffreqs[] = {
	{MHZ(19.2),		MHZ(19.2),		MHZ(19.2) * 7},
	{MHZ(22.0),		MHZ(22.0),		MHZ(22.0) * 6},
//	{MHZ(24.576),		MHZ(24.576),		MHZ(24.576) *5},
	{MHZ(24.000),		MHZ(24.000),		MHZ(24.000) *5},
	{MHZ(26),		MHZ(26),		MHZ(26) * 5},
	{MHZ(38.4),		MHZ(38.4)/2,		MHZ(38.4) * 3.5}
};

static void writereg(void *dev, uint8_t reg, uint32_t val) {
	fprintf(stderr, "%u 0x%08x\n", reg, val);
	mirisdr_reg_write_fn(dev, 0x09, val);
}

static int set_reg0(void *dev, struct state* s) {
	uint32_t reg0=0;

	reg0 = r0_modes[s->m].value << 4;

	if (s->m == AM_MODE1 || s->m == AM_MODE1) {
		reg0 |= FIL_MODE_450K_IF << R0_FIL_MODE_SH;
		reg0 |= 0x1 << R0_FIL_BW_SH;//hack filter bw
	} else {
		reg0 |= FIL_MODE_ZERO_IF << R0_FIL_MODE_SH;
		reg0 |= 0x7 << R0_FIL_BW_SH;//hack filter bw
	}

	reg0 |= s->x << R0_XTAL_SEL_SH;

	writereg(dev, 0, reg0);
	s->reg[0] = reg0;
	return 0;
}

static int set_reg52 (void *dev, struct state* s) {
	uint32_t reg5=5;
	uint32_t reg2=2;
	float i_want_a_fracstep = 1000e3;

	float f_if=0, fsynth,synthstep;
	uint32_t thresh,int_,frac;

	if (s->m == AM_MODE1 || s->m == AM_MODE1) {
		f_if = 450e3;
		fsynth = (iffreqs[s->x].fif1 - s->freq_hz + f_if) * r0_modes[s->m].lodiv;
		fprintf(stderr, "wtf is if2?");
		return -1;
	} else {
		fsynth = (s->freq_hz + f_if) * r0_modes[s->m].lodiv;
	}
	synthstep = i_want_a_fracstep * r0_modes[s->m].lodiv;
	thresh = (iffreqs[s->x].fref1*4)/synthstep;
	
	reg5 |= 0x28 << 16;
	reg5 |= (uint32_t)thresh << 4;

	int_ = fsynth/(iffreqs[s->x].fref1*4);//ROUNDDOWN?
	frac = ((fsynth/(iffreqs[s->x].fref1*4)) - int_)*thresh;

	reg2 |= frac << 4;
	reg2 |= int_ << R2_INT_SH;

	fprintf(stderr, "fsynth %fmhz synthstep %e thresh %u int_ %u frac %u // frac step value: %f\n", fsynth/1e6, synthstep, thresh, int_, frac, i_want_a_fracstep);
	writereg(dev, 5, reg5);
	writereg(dev, 2, reg2);
	/* bit 19 and 21 must be set */
	s->reg[5] = reg5;
	s->reg[2] = reg2;
	return 0;
}

static void setfreqs(void *dev, struct state* s) {
	c(set_reg0(dev, s));
	c(set_reg52(dev, s));
}

static int setgains(void *dev, struct state* s) {
	uint32_t reg1=1;
	s->minus_bbgain &= (1<<6)-1;
	reg1 |= s->minus_bbgain << 4;

	s->mixl &= 0x1;
	s->lnagr &= 0x1;

	if(s->m == AM_MODE1 || s->m == AM_MODE1){
		reg1 |= s->am_mixgainred << R1_MIXBU_SH;
	}
	else {
		s->am_mixgainred = r1_mixbu_p0_0;//not am = reset to 0
		reg1 |= s->lnagr << R1_LNAGR_SH;
	}
	reg1 |= s->mixl << R1_MIXL_SH;

	reg1 |= R1_DCCAL << R1_DCCAL_SH;//hack

	writereg(dev, 1, reg1);
	s->reg[1] = reg1;
	return 0;
}

static void checkfreq(void *dev, struct state* s) {
	uint32_t freq = (iffreqs[s->x].fref1*4)/r0_modes[s->m].lodiv;
	uint32_t frac = (s->reg[2] >> 4) & ((1<<12)-1);
	uint32_t int_ = (s->reg[2] >> R2_INT_SH) & ((1<<6)-1);
	uint32_t thresh = (s->reg[5] >> 4) & ((1<<12)-1);
	double res = freq * (float)((float)int_ + ((float)((frac << 12) +0)/(float)(thresh << 12) ));
	float fstep = (float)(iffreqs[s->x].fref1 * 4)/ r0_modes[s->m].lodiv * thresh;
	fprintf(stderr, "freq %u thresh %u int %u frac %u res: %fmhz\n", freq, thresh, int_, frac, res/1e6);
	fprintf(stderr, "+1 frac res: %fmhz\n", freq * (float)((float)int_ + ((float)(((frac+1) << 12) +0)/(float)(thresh << 12) )) /1e6);
	fprintf(stderr, "-1 frac res: %fmhz\n", freq * (float)((float)int_ + ((float)(((frac-1) << 12) +0)/(float)(thresh << 12) )) /1e6);
}

int msi001_init(void *dev, uint32_t freq)
{
	struct state curstate;

	memset(&curstate,0,sizeof(curstate));

	//band sel
	if (freq < MHZ(30))
		curstate.m = AM_MODE1;
	else if (freq < MHZ(140))
		curstate.m = VHF_MODE;
	else if (freq < MHZ(300))
		curstate.m = B3_MODE;
	else if (freq < MHZ(970))
		curstate.m = B45_MODE;
	else
		curstate.m = BL_MODE;

	curstate.x = XTAL24_576M;//xtal freq
	curstate.freq_hz= freq;

	setfreqs(dev, &curstate);

	curstate.minus_bbgain = 20;//gain reduction: 0-59dB
	curstate.am_mixgainred = r1_mixbu_p0_12;// ignored & reset except in am mode
	curstate.mixl = 0;// bool, table 6-11
	curstate.lnagr = 0;// bool, table 6-11

//	c(setgains(dev, &curstate));
	//no dc track timing
	//no aux features
	// AFC?

	checkfreq(dev, &curstate);
	return 0;
}

