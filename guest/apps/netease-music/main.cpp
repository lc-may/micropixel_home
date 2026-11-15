// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "music_activity.hpp"
int main() {
    micropixel::Application app;
    auto model = std::make_unique<netease::MusicModel>(app);
    netease::MusicActivity activity(*model);
    netease::PlaybackService service(*model);
    app.RunComponents(&activity, &service);
    return 0;
}
