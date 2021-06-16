//
// Copyright 2019 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2019 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

struct nni_plat_udp {
    SOCKET           s;
    nni_mtx          lk;
    nni_cv           cv;
    nni_list         rxq;
    nni_win_io       rxio;
    int              cancel_rv;
    bool             closed;
    SOCKADDR_STORAGE rxsa;
    int              rxsalen;
};
