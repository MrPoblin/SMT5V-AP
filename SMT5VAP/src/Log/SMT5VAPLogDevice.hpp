#pragma once

#include <DynamicOutput/OutputDevice.hpp>
#include <File/File.hpp>

namespace RC::Output
{
    class SMT5VAPLogDevice : public OutputDevice
    {
    private:
        mutable File::Handle m_file;
        mutable bool m_is_device_ready{};

    public:
        ~SMT5VAPLogDevice() override;

    public:
        auto has_optional_arg() const -> bool override;
        auto receive(File::StringViewType fmt) const -> void override;
        auto receive_with_optional_arg(File::StringViewType fmt, int32_t optional_arg = 0) const -> void override;

    private:
        auto start_device() const -> void;
    };
}
