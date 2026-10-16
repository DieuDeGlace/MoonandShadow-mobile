#pragma once
#include <d3d11.h>

// Whether a vertex constant-buffer binding has the native shader's buffer
// sizes. Buffer descriptions never change, so the answer for the last binding
// tuple is reused instead of calling GetDesc on every buffer of every draw. The
// tuple is retained, so a freed buffer's address cannot be mistaken for it.
// (Intentionally no destructor: nothing is released during process exit.)
template <unsigned N> class BufferLayout {
    ID3D11Buffer *retained[N]{};
    bool known = false, native = false;

  public:
    unsigned inspections = 0;
    void clear() {
        for (auto &buffer : retained) {
            if (buffer)
                buffer->Release();
            buffer = nullptr;
        }
        known = false;
    }
    bool matches(ID3D11Buffer *const *buffers, const UINT (&sizes)[N]) {
        bool same = known;
        for (unsigned i = 0; i < N && same; ++i)
            same = retained[i] == buffers[i];
        if (same)
            return native;
        clear();
        native = true;
        for (unsigned i = 0; i < N; ++i) {
            D3D11_BUFFER_DESC desc{};
            if ((retained[i] = buffers[i]) != nullptr) {
                retained[i]->AddRef();
                retained[i]->GetDesc(&desc);
            }
            native &= desc.ByteWidth == sizes[i];
        }
        known = true;
        ++inspections;
        return native;
    }
};
