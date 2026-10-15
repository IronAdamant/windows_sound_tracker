#pragma once
#include <unknwn.h>

// Minimal RAII holder for COM interface pointers (WRL is not available on every toolchain).
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* attach) : m_p(attach) {}
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : m_p(other.m_p) { other.m_p = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            m_p = other.m_p;
            other.m_p = nullptr;
        }
        return *this;
    }

    T* Get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

    // Releases the current pointer and returns the address for an out-parameter.
    T** Put() {
        Reset();
        return &m_p;
    }

    void Reset() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

    template <class U>
    HRESULT As(ComPtr<U>& out) const {
        if (!m_p) return E_POINTER;
        return m_p->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.Put()));
    }

private:
    T* m_p = nullptr;
};
