#include <iostream>
import kanariv.media;

int main() {
    auto tracks = kanariv::media::scanTracksToVector(".");
    std::cout << (tracks ? tracks->size() : 0) << " tracks\n";
}
