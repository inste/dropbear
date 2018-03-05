#include "fuzz.h"
#include "session.h"
#include "fuzz-wrapfd.h"
#include "debug.h"
#include "runopts.h"
#include "algo.h"
#include "bignum.h"

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
	static int once = 0;
	static const struct dropbear_kex *ecdh[3]; /* 256, 384, 521 */
	static struct key_context* keep_newkeys = NULL;
	#define NUM_PARAMS 800
	static struct kex_ecdh_param *ecdh_params[NUM_PARAMS];

	if (!once) {
		fuzz_common_setup();
		fuzz_svr_setup();

		/* ses gets zeroed by fuzz_set_input */
		keep_newkeys = (struct key_context*)m_malloc(sizeof(struct key_context));
		ecdh[0] = fuzz_get_algo(sshkex, "ecdh-sha2-nistp256");
		ecdh[1] = fuzz_get_algo(sshkex, "ecdh-sha2-nistp384");
		ecdh[2] = fuzz_get_algo(sshkex, "ecdh-sha2-nistp521");
		assert(ecdh[0]);
		assert(ecdh[1]);
		assert(ecdh[2]);
		keep_newkeys->algo_hostkey = DROPBEAR_SIGNKEY_ECDSA_NISTP256;
		ses.newkeys = keep_newkeys;

		/* Pre-generate parameters */
		int i;
		for (i = 0; i < NUM_PARAMS; i++) {
			ses.newkeys->algo_kex = ecdh[i % 3];
			ecdh_params[i] = gen_kexecdh_param();
		}

		once = 1;
	}

	if (fuzz_set_input(Data, Size) == DROPBEAR_FAILURE) {
		return 0;
	}

	m_malloc_set_epoch(1);

	if (setjmp(fuzz.jmp) == 0) {
		/* Based on recv_msg_kexdh_init()/send_msg_kexdh_reply() 
		with DROPBEAR_KEX_ECDH */
		ses.newkeys = keep_newkeys;

		/* random choice of ecdh 256, 384, 521 */
		unsigned char b = buf_getbyte(fuzz.input);
		ses.newkeys->algo_kex = ecdh[b % 3];

		/* Choose from the collection of ecdh params */
		unsigned int e = buf_getint(fuzz.input);
		struct kex_ecdh_param *ecdh_param = ecdh_params[e % NUM_PARAMS];

		buffer * ecdh_qs = buf_getstringbuf(fuzz.input);

		ses.kexhashbuf = buf_new(4);
		buf_putint(ses.kexhashbuf, 12345);
		kexecdh_comb_key(ecdh_param, ecdh_qs, svr_opts.hostkey);

		/* kexhashbuf is freed in kexdh_comb_key */
		m_free(ses.dh_K);
		buf_free(ecdh_qs);

		m_malloc_free_epoch(1, 0);
	} else {
		m_malloc_free_epoch(1, 1);
		TRACE(("dropbear_exit longjmped"))
		/* dropbear_exit jumped here */
	}

	return 0;
}