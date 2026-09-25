#include "proto.hpp"

#include <cstring>

namespace proto {

uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= static_cast<uint16_t>(*p++) << 8;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc)
{
    // Tabela de 16 entradas (meio byte por vez): pequena e rápida o bastante para 4 KB
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ T[crc & 0x0F];
        crc = (crc >> 4) ^ T[crc & 0x0F];
    }
    return ~crc;
}

size_t seal(uint8_t *out, Channel ch, uint8_t op, uint16_t seq, uint16_t len)
{
    out[0] = SOF0;
    out[1] = SOF1;
    out[2] = ch;
    out[3] = op;
    out[4] = static_cast<uint8_t>(seq);
    out[5] = static_cast<uint8_t>(seq >> 8);
    out[6] = static_cast<uint8_t>(len);
    out[7] = static_cast<uint8_t>(len >> 8);
    const uint16_t crc = crc16(out + 2, HEADER - 2 + len);
    out[HEADER + len]     = static_cast<uint8_t>(crc);
    out[HEADER + len + 1] = static_cast<uint8_t>(crc >> 8);
    return HEADER + len + TRAILER;
}

size_t encode(uint8_t *out, Channel ch, uint8_t op, uint16_t seq, const void *payload, uint16_t len)
{
    if (len) memmove(out + HEADER, payload, len);
    return seal(out, ch, op, seq, len);
}

bool Parser::feed(uint8_t b, Frame &out)
{
    switch (st_) {
    case S_SOF0:
        if (b == SOF0) st_ = S_SOF1;
        return false;
    case S_SOF1:
        if (b == SOF1) {
            buf_[0] = SOF0;
            buf_[1] = SOF1;
            n_      = 2;
            st_     = S_HDR;
        } else if (b != SOF0) {
            st_ = S_SOF0;
        }
        return false;
    case S_HDR:
        buf_[n_++] = b;
        if (n_ == HEADER) {
            const size_t len = buf_[6] | (buf_[7] << 8);
            if (len > MAX_PAYLOAD) {
                ++bad_;
                st_ = S_SOF0;
                return false;
            }
            end_ = HEADER + len + TRAILER;
            st_  = S_BODY;
        }
        return false;
    case S_BODY:
        buf_[n_++] = b;
        if (n_ < end_) return false;
        st_ = S_SOF0;
        {
            const size_t   len = end_ - HEADER - TRAILER;
            const uint16_t crc = buf_[end_ - 2] | (buf_[end_ - 1] << 8);
            if (crc16(buf_ + 2, HEADER - 2 + len) != crc) {
                ++bad_;
                return false;
            }
            out.ch      = buf_[2];
            out.op      = buf_[3];
            out.seq     = static_cast<uint16_t>(buf_[4] | (buf_[5] << 8));
            out.len     = static_cast<uint16_t>(len);
            out.payload = buf_ + HEADER;
        }
        return true;
    }
    return false;
}

}  // namespace proto
