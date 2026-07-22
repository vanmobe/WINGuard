#ifndef WINGCONNECTOR_INTERNAL_PLAYBACK_NAMING_H
#define WINGCONNECTOR_INTERNAL_PLAYBACK_NAMING_H

#include <string>

namespace WingConnector::PlaybackNaming {

inline std::string StereoBaseName(std::string name) {
    while (name.size() >= 4) {
        const std::string suffix = name.substr(name.size() - 4);
        if (suffix != " (L)" && suffix != " (R)") {
            break;
        }
        name.resize(name.size() - 4);
    }
    return name;
}

inline std::string StereoInputName(const std::string& base_name) {
    return StereoBaseName(base_name);
}

inline std::string StereoOutputLeftName(const std::string& base_name) {
    return StereoBaseName(base_name);
}

inline std::string StereoOutputRightName(const std::string& base_name) {
    return StereoBaseName(base_name);
}

}  // namespace WingConnector::PlaybackNaming

#endif
