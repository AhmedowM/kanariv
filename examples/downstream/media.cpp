#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    std::cout << std::boolalpha
              << kanariv::media::hasAudioExt("track.flac") << '\n';
}
