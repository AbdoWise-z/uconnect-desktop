#include "core/Bunzip2.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ucd {

namespace {

// bzip2's CRC-32: polynomial 0x04C11DB7, most significant bit first.
const std::array<uint32_t, 256>& crcTable() {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

struct Failure {
    const char* what;
};

class BitReader {
public:
    explicit BitReader(const QByteArray& data) : data_(reinterpret_cast<const uint8_t*>(data.constData())), size_(data.size()) {}

    uint32_t bits(int count) {
        uint32_t value = 0;
        while (count-- > 0) value = (value << 1) | bit();
        return value;
    }
    uint32_t bit() {
        if (byte_ >= size_) throw Failure{"truncated"};
        const uint32_t b = (data_[byte_] >> (7 - shift_)) & 1u;
        if (++shift_ == 8) {
            shift_ = 0;
            ++byte_;
        }
        return b;
    }
    void alignToByte() {
        if (shift_) {
            shift_ = 0;
            ++byte_;
        }
    }
    bool atEnd() const { return byte_ >= size_; }

private:
    const uint8_t* data_;
    qsizetype      size_;
    qsizetype      byte_ = 0;
    int            shift_ = 0;
};

// One canonical Huffman table, as bzip2 assigns codes: by length, then symbol.
struct Huffman {
    std::array<int, 21>  count{}, firstCode{}, firstIndex{};
    std::vector<int>     symbols;  // ordered by (length, symbol)

    explicit Huffman(const std::vector<int>& lengths) {
        for (int length : lengths) ++count[length];
        for (int length = 1; length <= 20; ++length)
            for (size_t s = 0; s < lengths.size(); ++s)
                if (lengths[s] == length) symbols.push_back(int(s));
        int code = 0, index = 0;
        for (int length = 1; length <= 20; ++length) {
            firstCode[length]  = code;
            firstIndex[length] = index;
            code  = (code + count[length]) << 1;
            index += count[length];
        }
    }
    int decode(BitReader& in) const {
        int code = 0;
        for (int length = 1; length <= 20; ++length) {
            code = (code << 1) | int(in.bit());
            const int offset = code - firstCode[length];
            if (count[length] && offset >= 0 && offset < count[length]) return symbols[size_t(firstIndex[length] + offset)];
        }
        throw Failure{"bad Huffman code"};
    }
};

// Decodes one block into `out`; returns its CRC as computed.
uint32_t decodeBlock(BitReader& in, int maxBlock, QByteArray& out, qsizetype maxSize) {
    const uint32_t storedCrc = in.bits(32);
    if (in.bit()) throw Failure{"randomised blocks are not supported"};
    const uint32_t origin = in.bits(24);

    // The byte values the block uses.
    std::vector<uint8_t> used;
    const uint32_t       groupsUsed = in.bits(16);
    for (int g = 0; g < 16; ++g)
        if (groupsUsed & (0x8000u >> g)) {
            const uint32_t bitsUsed = in.bits(16);
            for (int b = 0; b < 16; ++b)
                if (bitsUsed & (0x8000u >> b)) used.push_back(uint8_t(g * 16 + b));
        }
    if (used.empty()) throw Failure{"block uses no symbols"};
    const int alphabet = int(used.size()) + 2;  // + RUNA/RUNB, EOB

    const int tables = int(in.bits(3));
    if (tables < 2 || tables > 6) throw Failure{"bad table count"};
    const int selectorCount = int(in.bits(15));
    if (selectorCount < 1) throw Failure{"no selectors"};
    std::vector<int> selectors(static_cast<size_t>(selectorCount));
    std::array<int, 6> tableOrder{0, 1, 2, 3, 4, 5};
    for (int& selector : selectors) {
        int index = 0;
        while (in.bit())
            if (++index >= tables) throw Failure{"bad selector"};
        const int table = tableOrder[size_t(index)];
        for (int k = index; k > 0; --k) tableOrder[size_t(k)] = tableOrder[size_t(k - 1)];
        tableOrder[0] = table;
        selector      = table;
    }

    std::vector<Huffman> huffman;
    for (int t = 0; t < tables; ++t) {
        std::vector<int> lengths(static_cast<size_t>(alphabet));
        int              length = int(in.bits(5));
        for (int& l : lengths) {
            while (in.bit()) length += in.bit() ? -1 : 1;
            if (length < 1 || length > 20) throw Failure{"bad code length"};
            l = length;
        }
        huffman.emplace_back(lengths);
    }

    // Huffman -> move-to-front + run lengths -> the BWT block.
    std::vector<uint8_t> block;
    block.reserve(size_t(maxBlock));
    std::array<uint8_t, 256> mtf{};
    for (int i = 0; i < 256; ++i) mtf[size_t(i)] = uint8_t(i);
    std::array<uint32_t, 256> counts{};
    int  symbolsLeft = 0, selector = 0;
    long run = 0, runWeight = 1;
    const Huffman* table = nullptr;
    const auto flushRun = [&] {
        if (!run) return;
        if (long(block.size()) + run > maxBlock) throw Failure{"block too large"};
        const uint8_t value = used[mtf[0]];
        block.insert(block.end(), size_t(run), value);
        counts[value] += uint32_t(run);
        run       = 0;
        runWeight = 1;
    };
    for (;;) {
        if (symbolsLeft-- == 0) {
            if (selector >= selectorCount) throw Failure{"ran out of selectors"};
            table       = &huffman[size_t(selectors[size_t(selector++)])];
            symbolsLeft = 49;
        }
        const int symbol = table->decode(in);
        if (symbol <= 1) {  // RUNA / RUNB: bijective base-2 run length
            if (runWeight > (1L << 22)) throw Failure{"run too long"};
            run += (symbol + 1) * runWeight;
            runWeight <<= 1;
            continue;
        }
        flushRun();
        if (symbol == alphabet - 1) break;  // end of block
        const int index = symbol - 1;
        const uint8_t front = mtf[size_t(index)];
        for (int k = index; k > 0; --k) mtf[size_t(k)] = mtf[size_t(k - 1)];
        mtf[0] = front;
        if (long(block.size()) >= maxBlock) throw Failure{"block too large"};
        const uint8_t value = used[front];
        block.push_back(value);
        ++counts[value];
    }
    if (origin >= block.size()) throw Failure{"bad origin pointer"};

    // Inverse Burrows-Wheeler.
    std::array<uint32_t, 256> start{};
    for (int i = 1; i < 256; ++i) start[size_t(i)] = start[size_t(i - 1)] + counts[size_t(i - 1)];
    std::vector<uint32_t> next(block.size());
    for (uint32_t i = 0; i < block.size(); ++i) next[start[block[i]]++] = i;

    // Undo the initial run-length step (4 equal bytes, then a count), with the CRC.
    const auto& crc = crcTable();
    uint32_t    blockCrc = 0xFFFFFFFFu;
    const auto  put = [&](uint8_t byte) {
        if (out.size() >= maxSize) throw Failure{"output too large"};
        out.append(char(byte));
        blockCrc = (blockCrc << 8) ^ crc[((blockCrc >> 24) ^ byte) & 0xFF];
    };
    uint32_t position = next[origin];
    int      last = -1, repeats = 0;
    for (size_t produced = 0; produced < block.size(); ++produced) {
        const uint8_t byte = block[position];
        position           = next[position];
        if (repeats == 4) {
            for (int k = 0; k < byte; ++k) put(uint8_t(last));
            repeats = 0;
            last    = -1;
            continue;
        }
        repeats = byte == last ? repeats + 1 : 1;
        last    = byte;
        put(byte);
    }
    blockCrc = ~blockCrc;
    if (blockCrc != storedCrc) throw Failure{"block CRC mismatch"};
    return blockCrc;
}

}  // namespace

std::optional<QByteArray> bunzip2(const QByteArray& compressed, qsizetype maxSize, QString* error) {
    QByteArray out;
    try {
        BitReader in(compressed);
        do {
            if (in.bits(8) != 'B' || in.bits(8) != 'Z' || in.bits(8) != 'h') throw Failure{"not a bzip2 stream"};
            const uint32_t level = in.bits(8);
            if (level < '1' || level > '9') throw Failure{"bad block size"};
            const int maxBlock = int(level - '0') * 100000;
            uint32_t  combined = 0;
            for (;;) {
                const uint64_t magic = (uint64_t(in.bits(24)) << 24) | in.bits(24);
                if (magic == 0x314159265359ull) {
                    const uint32_t blockCrc = decodeBlock(in, maxBlock, out, maxSize);
                    combined = ((combined << 1) | (combined >> 31)) ^ blockCrc;
                } else if (magic == 0x177245385090ull) {
                    if (in.bits(32) != combined) throw Failure{"stream CRC mismatch"};
                    in.alignToByte();
                    break;
                } else {
                    throw Failure{"bad block header"};
                }
            }
        } while (!in.atEnd());
    } catch (const Failure& failure) {
        if (error) *error = QString::fromLatin1(failure.what);
        return std::nullopt;
    }
    return out;
}

}  // namespace ucd
