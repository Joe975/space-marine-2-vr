#pragma once
#include <wrl/client.h>

// Keep the successfully checked queue alive: its address cannot be reused for an
// unchecked object. A queue replacement, runtime reset or compositor error clears it.
template<class Queue>
class ValidatedQueue {
public:
    template<class Validate>
    bool accepts(Queue* queue, Validate&& validate) {
        if (!queue) { return false; }
        if (m_queue.Get() == queue) { return true; }
        m_queue.Reset();
        if (!validate(queue)) { return false; }
        m_queue = queue;
        return true;
    }
    void reset() { m_queue.Reset(); }
private:
    Microsoft::WRL::ComPtr<Queue> m_queue;
};
