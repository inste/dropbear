/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "session.h"
#include "dbutil.h"
#include "algo.h"
#include "buffer.h"
#include "session.h"
#include "kex.h"
#include "ssh.h"
#include "packet.h"
#include "bignum.h"
#include "random.h"
#include "runopts.h"
#include "signkey.h"


static void checkhostkey(unsigned char* keyblob, unsigned int keybloblen);
#define MAX_KNOWNHOSTS_LINE 4500

void send_msg_kexdh_init() {

	cli_ses.dh_e = (mp_int*)m_malloc(sizeof(mp_int));
	cli_ses.dh_x = (mp_int*)m_malloc(sizeof(mp_int));

	m_mp_init_multi(cli_ses.dh_e, cli_ses.dh_x, NULL);
	gen_kexdh_vals(cli_ses.dh_e, cli_ses.dh_x);

	CHECKCLEARTOWRITE();
	buf_putbyte(ses.writepayload, SSH_MSG_KEXDH_INIT);
	buf_putmpint(ses.writepayload, cli_ses.dh_e);
	encrypt_packet();
	ses.requirenext = SSH_MSG_KEXDH_REPLY;
}

/* Handle a diffie-hellman key exchange reply. */
void recv_msg_kexdh_reply() {

	mp_int dh_f;
	sign_key *hostkey = NULL;
	unsigned int type, keybloblen;
	unsigned char* keyblob = NULL;


	TRACE(("enter recv_msg_kexdh_reply"));
	type = ses.newkeys->algo_hostkey;
	TRACE(("type is %d", type));

	hostkey = new_sign_key();
	keybloblen = buf_getint(ses.payload);

	keyblob = buf_getptr(ses.payload, keybloblen);
	if (!ses.kexstate.donefirstkex) {
		/* Only makes sense the first time */
		checkhostkey(keyblob, keybloblen);
	}

	if (buf_get_pub_key(ses.payload, hostkey, &type) != DROPBEAR_SUCCESS) {
		TRACE(("failed getting pubkey"));
		dropbear_exit("Bad KEX packet");
	}

	m_mp_init(&dh_f);
	if (buf_getmpint(ses.payload, &dh_f) != DROPBEAR_SUCCESS) {
		TRACE(("failed getting mpint"));
		dropbear_exit("Bad KEX packet");
	}

	kexdh_comb_key(cli_ses.dh_e, cli_ses.dh_x, &dh_f, hostkey);
	mp_clear(&dh_f);

	if (buf_verify(ses.payload, hostkey, ses.hash, SHA1_HASH_SIZE) 
			!= DROPBEAR_SUCCESS) {
		dropbear_exit("Bad hostkey signature");
	}

	sign_key_free(hostkey);
	hostkey = NULL;

	send_msg_newkeys();
	ses.requirenext = SSH_MSG_NEWKEYS;
	TRACE(("leave recv_msg_kexdh_init"));
}

static void ask_to_confirm(unsigned char* keyblob, unsigned int keybloblen) {

	char* fp = NULL;

	fp = sign_key_fingerprint(keyblob, keybloblen);
	fprintf(stderr, "\nHost '%s' is not in the trusted hosts file.\n(fingerprint %s)\nDo you want to continue connecting? (y/n)\n", 
			cli_opts.remotehost, 
			fp);

	if (getc(stdin) == 'y') {
		m_free(fp);
		return;
	}

	dropbear_exit("Didn't validate host key");
}

static void checkhostkey(unsigned char* keyblob, unsigned int keybloblen) {

	char * filename = NULL;
	FILE *hostsfile = NULL;
	struct passwd *pw = NULL;
	unsigned int len, hostlen;
	const char *algoname = NULL;
	buffer * line = NULL;
	int ret;
	
	pw = getpwuid(getuid());

	if (pw == NULL) {
		dropbear_exit("Failed to get homedir");
	}

	len = strlen(pw->pw_dir);
	filename = m_malloc(len + 18); /* "/.ssh/known_hosts" and null-terminator*/

	snprintf(filename, len+18, "%s/.ssh", pw->pw_dir);
	/* Check that ~/.ssh exists - easiest way is just to mkdir */
	if (mkdir(filename, S_IRWXU) != 0) {
		if (errno != EEXIST) {
			ask_to_confirm(keyblob, keybloblen);
			goto out; /* only get here on success */
		}
	}

	snprintf(filename, len+18, "%s/.ssh/known_hosts", pw->pw_dir);
	hostsfile = fopen(filename, "r+");
	if (hostsfile == NULL) {
		ask_to_confirm(keyblob, keybloblen);
		goto out; /* We only get here on success */
	}

	line = buf_new(MAX_KNOWNHOSTS_LINE);
	hostlen = strlen(cli_opts.remotehost);

	do {
		if (buf_getline(line, hostsfile) == DROPBEAR_FAILURE) {
			TRACE(("failed reading line: prob EOF"));
			break;
		}

		/* The line is too short to be sensible */
		/* "30" is 'enough to hold ssh-dss plus the spaces, ie so we don't
		 * buf_getfoo() past the end and die horribly - the base64 parsing
		 * code is what tiptoes up to the end nicely */
		if (line->len < (hostlen+30) ) {
			TRACE(("line is too short to be sensible"));
			continue;
		}

		/* Compare hostnames */
		if (strncmp(cli_opts.remotehost, buf_getptr(line, hostlen),
					hostlen) != 0) {
			TRACE(("hosts don't match"));
			continue;
		}

		buf_incrpos(line, hostlen);
		if (buf_getbyte(line) != ' ') {
			/* there wasn't a space after the hostname, something dodgy */
			TRACE(("missing space afte matching hostname"));
			continue;
		}

		algoname = signkey_name_from_type(ses.newkeys->algo_hostkey, &len);
		if ( strncmp(buf_getptr(line, len), algoname, len) != 0) {
			TRACE(("algo doesn't match"));
			continue;
		}

		buf_incrpos(line, len);
		if (buf_getbyte(line) != ' ') {
			TRACE(("missing space after algo"));
			continue;
		}

		/* Now we're at the interesting hostkey */
		ret = cmp_base64_key(keyblob, keybloblen, algoname, len, line);

		if (ret == DROPBEAR_SUCCESS) {
			/* Good matching key */
			TRACE(("good matching key"));
			goto out;
		}

		/* The keys didn't match. eep. */
	} while (1); /* keep going 'til something happens */

	/* Key doesn't exist yet */
	ask_to_confirm(keyblob, keybloblen);
	/* If we get here, they said yes */

out:
	if (hostsfile != NULL) {
		fclose(hostsfile);
	}
	m_free(filename);
	buf_free(line);
}
