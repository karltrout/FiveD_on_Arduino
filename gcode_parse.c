#include	"gcode_parse.h"

#include	<string.h>

#include	"serial.h"
#include	"sermsg.h"
#include	"dda_queue.h"
#include	"debug.h"
#include	"heater.h"
#include	"sersendf.h"

#include	"gcode_process.h"

/*
	Switch user friendly values to coding friendly values

	This also affects the possible build volume. We have +/- 2^31 numbers available and as we internally measure position in steps and use a precision factor of 1000, this translates into a possible range of

		2^31 mm / STEPS_PER_MM_x / 1000

	for each axis. For a M6 threaded rod driven machine and 1/16 microstepping this evaluates to

		2^31 mm / 200 / 16 / 1000 = 671 mm,

	which is about the worst case we have. All other machines have a bigger build volume.
*/

#define	STEPS_PER_M_X			((uint32_t) ((STEPS_PER_MM_X * 1000.0) + 0.5))
#define	STEPS_PER_M_Y			((uint32_t) ((STEPS_PER_MM_Y * 1000.0) + 0.5))
#define	STEPS_PER_M_Z			((uint32_t) ((STEPS_PER_MM_Z * 1000.0) + 0.5))
#define	STEPS_PER_M_E			((uint32_t) ((STEPS_PER_MM_E * 1000.0) + 0.5))

/*
	mm -> inch conversion
*/

#define	STEPS_PER_IN_X		((uint32_t) ((25.4 * STEPS_PER_MM_X) + 0.5))
#define	STEPS_PER_IN_Y		((uint32_t) ((25.4 * STEPS_PER_MM_Y) + 0.5))
#define	STEPS_PER_IN_Z		((uint32_t) ((25.4 * STEPS_PER_MM_Z) + 0.5))
#define	STEPS_PER_IN_E		((uint32_t) ((25.4 * STEPS_PER_MM_E) + 0.5))

uint8_t last_field = 0;

#define crc(a, b)		(a ^ b)

decfloat read_digit					__attribute__ ((__section__ (".bss")));

GCODE_COMMAND next_target		__attribute__ ((__section__ (".bss")));

/*
	utility functions
*/
extern const uint32_t powers[];  // defined in sermsg.c
const int32_t rounding[DECFLOAT_EXP_MAX] = {0,  5,  50,  500,  5000,  50000,  500000};

static int32_t decfloat_to_int(decfloat *df, int32_t multiplicand, uint32_t denominator) {
	int32_t	r = df->mantissa;
	uint8_t	e = df->exponent;

	// e=1 means we've seen a decimal point but no digits after it, and e=2 means we've seen a decimal point with one digit so it's too high by one if not zero
	if (e)
		e--;

	int32_t	rnew1 = r * (multiplicand / denominator);
	if (e)
	{
		int32_t	rnew2 = r * (multiplicand % denominator) / denominator;
		r = rnew1 + rnew2;

		r = (r + rounding[e]) / powers[e];
	}
	else
	{
		int32_t	rnew2 = (r * (multiplicand % denominator) + (denominator / 2)) / denominator;
		r = rnew1 + rnew2;
	}

	return df->sign ? -r : r;
}

/****************************************************************************
*                                                                           *
* Character Received - add it to our command                                *
*                                                                           *
****************************************************************************/

void gcode_parse_char(uint8_t c) {
	#ifdef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, c);
	#endif

	// uppercase
	if (c >= 'a' && c <= 'z')
		c &= ~32;

	// process previous field
	if (last_field) {
		// check if we're seeing a new field or end of line
		// any character will start a new field, even invalid/unknown ones
		if ((c >= 'A' && c <= 'Z') || c == '*' || (c == 10) || (c == 13)) {
			switch (last_field) {
				case 'G':
					next_target.G = read_digit.mantissa;
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint8(next_target.G);
					break;
				case 'M':
					next_target.M = read_digit.mantissa;
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint8(next_target.M);
					break;
				case 'X':
					if (next_target.option_inches)
						next_target.target.X = decfloat_to_int(&read_digit, STEPS_PER_IN_X, 1);
					else
						next_target.target.X = decfloat_to_int(&read_digit, STEPS_PER_M_X, 1000);
					if (debug_flags & DEBUG_ECHO)
						serwrite_int32(next_target.target.X);
					break;
				case 'Y':
					if (next_target.option_inches)
						next_target.target.Y = decfloat_to_int(&read_digit, STEPS_PER_IN_Y, 1);
					else
						next_target.target.Y = decfloat_to_int(&read_digit, STEPS_PER_M_Y, 1000);
					if (debug_flags & DEBUG_ECHO)
						serwrite_int32(next_target.target.Y);
					break;
				case 'Z':
					if (next_target.option_inches)
						next_target.target.Z = decfloat_to_int(&read_digit, STEPS_PER_IN_Z, 1);
					else
						next_target.target.Z = decfloat_to_int(&read_digit, STEPS_PER_M_Z, 1000);
					if (debug_flags & DEBUG_ECHO)
						serwrite_int32(next_target.target.Z);
					break;
				case 'E':
					if (next_target.option_inches)
						next_target.target.E = decfloat_to_int(&read_digit, STEPS_PER_IN_E, 1);
					else
						next_target.target.E = decfloat_to_int(&read_digit, STEPS_PER_M_E, 1000);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint32(next_target.target.E);
					break;
				case 'F':
					// just use raw integer, we need move distance and n_steps to convert it to a useful value, so wait until we have those to convert it
					if (next_target.option_inches)
						next_target.target.F = decfloat_to_int(&read_digit, 254, 10);
					else
						next_target.target.F = decfloat_to_int(&read_digit, 1, 1);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint32(next_target.target.F);
					break;
				case 'S':
					// if this is temperature, multiply by 4 to convert to quarter-degree units
					// cosmetically this should be done in the temperature section,
					// but it takes less code, less memory and loses no precision if we do it here instead
					if ((next_target.M == 104) || (next_target.M == 109))
						next_target.S = decfloat_to_int(&read_digit, 4, 1);
					// if this is heater PID stuff, multiply by PID_SCALE because we divide by PID_SCALE later on
					else if ((next_target.M >= 130) && (next_target.M <= 132))
						next_target.S = decfloat_to_int(&read_digit, PID_SCALE, 1);
					else
						next_target.S = decfloat_to_int(&read_digit, 1, 1);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint16(next_target.S);
					break;
				case 'P':
					// if this is dwell, multiply by 1000 to convert seconds to milliseconds
					if (next_target.G == 4)
						next_target.P = decfloat_to_int(&read_digit, 1000, 1);
					else
						next_target.P = decfloat_to_int(&read_digit, 1, 1);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint16(next_target.P);
					break;
				case 'T':
					next_target.T = read_digit.mantissa;
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint8(next_target.T);
					break;
				case 'N':
					next_target.N = decfloat_to_int(&read_digit, 1, 1);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint32(next_target.N);
					break;
				case '*':
					next_target.checksum_read = decfloat_to_int(&read_digit, 1, 1);
					if (debug_flags & DEBUG_ECHO)
						serwrite_uint8(next_target.checksum_read);
					break;
			}
			// reset for next field
			last_field = 0;
			read_digit.sign = read_digit.mantissa = read_digit.exponent = 0;
		}
	}

	// skip comments
	if (next_target.seen_semi_comment == 0 && next_target.seen_parens_comment == 0) {
		// new field?
		if ((c >= 'A' && c <= 'Z') || c == '*') {
			last_field = c;
			if (debug_flags & DEBUG_ECHO)
				serial_writechar(c);
		}

		// process character
		switch (c) {
			// each currently known command is either G or M, so preserve previous G/M unless a new one has appeared
			// FIXME: same for T command
			case 'G':
				next_target.seen_G = 1;
				next_target.seen_M = 0;
				next_target.M = 0;
				break;
			case 'M':
				next_target.seen_M = 1;
				next_target.seen_G = 0;
				next_target.G = 0;
				break;
			case 'X':
				next_target.seen_X = 1;
				break;
			case 'Y':
				next_target.seen_Y = 1;
				break;
			case 'Z':
				next_target.seen_Z = 1;
				break;
			case 'E':
				next_target.seen_E = 1;
				break;
			case 'F':
				next_target.seen_F = 1;
				break;
			case 'S':
				next_target.seen_S = 1;
				break;
			case 'P':
				next_target.seen_P = 1;
				break;
			case 'T':
				next_target.seen_T = 1;
				break;
			case 'N':
				next_target.seen_N = 1;
				break;
			case '*':
				next_target.seen_checksum = 1;
				break;

			// comments
			case ';':
				next_target.seen_semi_comment = 1;
				break;
			case '(':
				next_target.seen_parens_comment = 1;
				break;

			// now for some numeracy
			case '-':
				read_digit.sign = 1;
				// force sign to be at start of number, so 1-2 = -2 instead of -12
				read_digit.exponent = 0;
				read_digit.mantissa = 0;
				break;
			case '.':
				if (read_digit.exponent == 0)
					read_digit.exponent = 1;
				break;
			#ifdef	DEBUG
			case ' ':
			case '\t':
			case 10:
			case 13:
				// ignore
				break;
			#endif

			default:
				// can't do ranges in switch..case, so process actual digits here.
				if (    c >= '0'
				     && c <= '9'
				     && read_digit.mantissa < (DECFLOAT_MANT_MAX / 10)
				     && read_digit.exponent < DECFLOAT_EXP_MAX ) {
					// this is simply mantissa = (mantissa * 10) + atoi(c) in different clothes
					read_digit.mantissa = (read_digit.mantissa << 3) + (read_digit.mantissa << 1) + (c - '0');
					if (read_digit.exponent)
						read_digit.exponent++;
				}
				#ifdef	DEBUG
				else {
					// invalid
					serial_writechar('?');
					serial_writechar(c);
					serial_writechar('?');
				}
				#endif
		}
	} else if ( next_target.seen_parens_comment == 1 && c == ')')
		next_target.seen_parens_comment = 0; // recognize stuff after a (comment)


	#ifndef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, c);
	#endif

	// end of line
	if ((c == 10) || (c == 13)) {
		if (debug_flags & DEBUG_ECHO)
			serial_writechar(c);

		if (
		#ifdef	REQUIRE_LINENUMBER
			(next_target.N >= next_target.N_expected) && (next_target.seen_N == 1)
		#else
			1
		#endif
			) {
			if (
				#ifdef	REQUIRE_CHECKSUM
				((next_target.checksum_calculated == next_target.checksum_read) && (next_target.seen_checksum == 1))
				#else
				((next_target.checksum_calculated == next_target.checksum_read) || (next_target.seen_checksum == 0))
				#endif
				) {
				// process
				serial_writestr_P(PSTR("ok "));
				process_gcode_command();
				serial_writestr_P(PSTR("\n"));

				// expect next line number
				if (next_target.seen_N == 1)
					next_target.N_expected = next_target.N + 1;
			}
			else {
				sersendf_P(PSTR("rs %ld Expected checksum %d\n"), next_target.N_expected, next_target.checksum_calculated);
				request_resend();
			}
		}
		else {
			sersendf_P(PSTR("rs %ld Expected line number %ld\n"), next_target.N_expected, next_target.N_expected);
			request_resend();
		}

		// reset variables
		next_target.seen_X = next_target.seen_Y = next_target.seen_Z = \
			next_target.seen_E = next_target.seen_F = next_target.seen_S = \
			next_target.seen_P = next_target.seen_T = next_target.seen_N = \
			next_target.seen_M = next_target.seen_checksum = next_target.seen_semi_comment = \
			next_target.seen_parens_comment = next_target.checksum_read = \
			next_target.checksum_calculated = 0;
		last_field = 0;
		read_digit.sign = read_digit.mantissa = read_digit.exponent = 0;

		// assume a G1 by default
		next_target.seen_G = 1;
		next_target.G = 1;

		if (next_target.option_relative) {
			next_target.target.X = next_target.target.Y = next_target.target.Z = 0;
		}
		// E always relative
		next_target.target.E = 0;
	}
}

/***************************************************************************\
*                                                                           *
* Request a resend of the current line - used from various places.          *
*                                                                           *
* Relies on the global variable next_target.N being valid.                  *
*                                                                           *
\***************************************************************************/

void request_resend(void) {
	serial_writestr_P(PSTR("rs "));
	serwrite_uint8(next_target.N);
	serial_writechar('\n');
}
