#ifndef __jack_sanitycheck_h__
#define __jack_sanitycheck_h__

/**
 * GPL etc.
 *
 * @author Florian Faber
 *
 * @version 0.1 (2009-01-17) [FF]
 *              - initial version
 **/

/**
 * Performs a range of sanity checks on the system. The number of
 * found problems is returned.
 *
 **/

#ifdef __cplusplus
extern "C" {
#endif

int sanitycheck (int do_realtime_check,
		 int do_freqscaling_check);

#ifdef __cplusplus
}
#endif

#endif /* __jack_sanitycheck_h__ */
