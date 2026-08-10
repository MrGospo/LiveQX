#pragma once
#include <memory>
#include <string>
#include "output/IOutput.h"

class RtspOutput : public IOutput {
public:
    explicit RtspOutput(const std::string& url);
    ~RtspOutput();

    void send(const Packet& pkt) override;
    bool isHealthy() const override;
    OutputStats getStats() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string url_;
    OutputStats stats_;
};
