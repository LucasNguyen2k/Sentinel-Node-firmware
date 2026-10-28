// Host stand-in: only MMFModule is needed by ezsn_hal.h (microphone tap module type).
#pragma once
class MMFModule {
public:
    virtual ~MMFModule() {}
protected:
    void *_p_mmf_context = nullptr;
};
