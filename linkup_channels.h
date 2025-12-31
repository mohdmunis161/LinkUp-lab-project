#ifndef LINKUP_CHANNELS_H
#define LINKUP_CHANNELS_H

#include <string>

struct ChannelSet {
    int chat_send_fd = -1;
    int chat_recv_fd = -1;
    int file_send_fds[5] = {-1,-1,-1,-1,-1};
    int file_recv_fds[5] = {-1,-1,-1,-1,-1};
    int audio_send_fd = -1;
    int audio_recv_fd = -1;
};

// Establish all 14 channels and return the ChannelSet
// Parameters: my_port, peer_port, peer_ip, role (m1/m2)
ChannelSet establish_channels(int my_main_port, int peer_main_port, const std::string& peer_ip, const std::string& role);

#endif // LINKUP_CHANNELS_H
