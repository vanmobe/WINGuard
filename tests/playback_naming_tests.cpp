#include "internal/playback_naming.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void TestStereoInputUsesBaseName() {
    Expect(WingConnector::PlaybackNaming::StereoInputName("OH") == "OH",
           "stereo inputs should keep the base track name");
}

void TestStereoOutputsUseSharedPairName() {
    Expect(WingConnector::PlaybackNaming::StereoOutputLeftName("OH") == "OH",
           "left stereo output should use the shared pair name");
    Expect(WingConnector::PlaybackNaming::StereoOutputRightName("OH") == "OH",
           "right stereo output should use the shared pair name");
}

void TestEmptyNamesStayPredictable() {
    Expect(WingConnector::PlaybackNaming::StereoInputName("") == "",
           "empty stereo input names should stay empty");
    Expect(WingConnector::PlaybackNaming::StereoOutputLeftName("") == "",
           "empty left output names should stay empty");
    Expect(WingConnector::PlaybackNaming::StereoOutputRightName("") == "",
           "empty right output names should stay empty");
}

void TestExistingSuffixesAreNormalizedBeforePairNaming() {
    Expect(WingConnector::PlaybackNaming::StereoInputName("OH (R)") == "OH",
           "stereo input names should remove an existing side suffix");
    Expect(WingConnector::PlaybackNaming::StereoOutputLeftName("OH (R)") == "OH",
           "left naming should remove an existing side suffix");
    Expect(WingConnector::PlaybackNaming::StereoOutputRightName("OH (L)") == "OH",
           "right naming should remove an existing side suffix");
    Expect(WingConnector::PlaybackNaming::StereoOutputRightName("OH (R) (R)") == "OH",
           "repeated side suffixes should collapse to the shared pair name");
}

}  // namespace

int main() {
    TestStereoInputUsesBaseName();
    TestStereoOutputsUseSharedPairName();
    TestEmptyNamesStayPredictable();
    TestExistingSuffixesAreNormalizedBeforePairNaming();
    std::cout << "playback_naming_tests: OK\n";
    return 0;
}
