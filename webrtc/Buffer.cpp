/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/ZLMediaKit/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdlib>
#include "Buffer.hpp"

StatisticImp(mediakit::Buffer)
StatisticImp(mediakit::BufferRaw)
StatisticImp(mediakit::BufferLikeString)

namespace mediakit {
BufferRaw::Ptr BufferRaw::create() {
    return Ptr(new BufferRaw);
}

}//namespace toolkit
