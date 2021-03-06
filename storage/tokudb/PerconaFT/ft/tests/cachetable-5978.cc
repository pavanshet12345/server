/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"



//
// This test verifies the behavior that originally caused
// #5978 is fixed. Here is what we do. We have four pairs with
// blocknums and fullhashes of 1,2,3,4. The cachetable has only
// two bucket mutexes, so 1 and 3 share a pair mutex, as do 2 and 4.
// We pin all four with expensive write locks. Then, on background threads,
// we call get_and_pin_nonblocking on 3, where the unlockers unpins 2, and
// we call get_and_pin_nonblocking on 4, where the unlockers unpins 1. Run this
// enough times, and we should see a deadlock before the fix, and no deadlock
// after the fix.
//

CACHEFILE f1;
PAIR p3;
PAIR p4;


static int
fetch_three (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(8);
    assert(k.b == 3);
    p3 = p;
    return 0;
}

static int
fetch_four (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(8);
    assert(k.b == 4);
    p4 = p;
    return 0;
}



static void
unpin_four (void* UU(v)) {
    int r = toku_cachetable_unpin_ct_prelocked_no_flush(
        f1,
        p3,
        CACHETABLE_DIRTY,
        make_pair_attr(8)
        );
    assert_zero(r);
}

static void
unpin_three (void* UU(v)) {
    int r = toku_cachetable_unpin_ct_prelocked_no_flush(
        f1,
        p4,
        CACHETABLE_DIRTY,
        make_pair_attr(8)
        );
    assert_zero(r);
}

static void *repin_one(void *UU(arg)) {
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    struct unlockers unlockers = {true, unpin_four, NULL, NULL};
    void* v1;
    int r = toku_cachetable_get_and_pin_nonblocking(
        f1,
        make_blocknum(1),
        1,
        &v1,
        wc,
        def_fetch,
        def_pf_req_callback,
        def_pf_callback,
        PL_WRITE_EXPENSIVE,
        NULL,
        &unlockers
        );
    assert(r == TOKUDB_TRY_AGAIN);
    return arg;
}


static void *repin_two(void *UU(arg)) {
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    struct unlockers unlockers = {true, unpin_three, NULL, NULL};
    void* v1;
    int r = toku_cachetable_get_and_pin_nonblocking(
        f1,
        make_blocknum(2),
        2,
        &v1,
        wc,
        def_fetch,
        def_pf_req_callback,
        def_pf_callback,
        PL_WRITE_EXPENSIVE,
        NULL,
        &unlockers
        );
    assert(r == TOKUDB_TRY_AGAIN);
    return arg;
}


static void
cachetable_test (void) {
    const int test_limit = 1000;
    int r;
    toku_pair_list_set_lock_size(2); // set two bucket mutexes
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);

    // bring pairs 1 and 2 into memory, then unpin
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 2, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);


    // now pin pairs 3 and 4
    r = toku_cachetable_get_and_pin(f1, make_blocknum(3), 3, &v1, wc, fetch_three, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(4), 4, &v1, wc, fetch_four, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);

    toku_pthread_t tid1;
    toku_pthread_t tid2;
    r = toku_pthread_create(
        toku_uninstrumented, &tid1, nullptr, repin_one, nullptr);
    assert_zero(r);
    r = toku_pthread_create(
        toku_uninstrumented, &tid2, nullptr, repin_two, nullptr);
    assert_zero(r);

    // unpin 1 and 2 so tid1 and tid2 can make progress
    usleep(512*1024);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, make_pair_attr(8));
    assert_zero(r);
    r = toku_test_cachetable_unpin(f1, make_blocknum(2), 2, CACHETABLE_DIRTY, make_pair_attr(8));
    assert_zero(r);


    void *ret;
    r = toku_pthread_join(tid1, &ret); 
    assert_zero(r);
    r = toku_pthread_join(tid2, &ret); 
    assert_zero(r);    

    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    // test ought to run bunch of times in hope of hitting bug
    uint32_t num_test_runs = 30;
    for (uint32_t i = 0; i < num_test_runs; i++) {
        if (verbose) {
            printf("starting test run %" PRIu32 " \n", i);
        }
        cachetable_test();
    }
    return 0;
}
