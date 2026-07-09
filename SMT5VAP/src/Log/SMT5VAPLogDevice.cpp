#include "SMT5VAPLogDevice.hpp"

namespace RC::Output
{
    SMT5VAPLogDevice::~SMT5VAPLogDevice()
    {
        if (m_file.is_valid())
        {
            m_file.close();
        }
    }

    auto SMT5VAPLogDevice::has_optional_arg() const -> bool
    {
        return true;
    }

    auto SMT5VAPLogDevice::start_device() const -> void
    {
        m_file = File::open(STR("SMT5VAP.log"), File::OpenFor::Appending, File::OverwriteExistingFile::Yes, File::CreateIfNonExistent::Yes);
        m_is_device_ready = true;
    }

    auto SMT5VAPLogDevice::receive(File::StringViewType fmt) const -> void
    {
        receive_with_optional_arg(fmt, 0);
    }

    auto SMT5VAPLogDevice::receive_with_optional_arg(File::StringViewType fmt, int32_t optional_arg) const -> void
    {
        if (!fmt.starts_with(STR("[SMT5VAP]"))) return;

        if (!m_is_device_ready)
        {
            start_device();
        }

        File::StringType formatted(fmt);
        size_t insert_pos = formatted.find(STR(']')) + 1;

        switch (optional_arg)
        {
        case LogLevel::Normal:
        case LogLevel::Default:
            formatted.insert(insert_pos, STR("[INFO]"));
            break;
        case LogLevel::Verbose:
            formatted.insert(insert_pos, STR("[DEBUG]"));
            break;
        case LogLevel::Warning:
            formatted.insert(insert_pos, STR("[WARN]"));
            break;
        case LogLevel::Error:
            formatted.insert(insert_pos, STR("[ERROR]"));
            break;
        }

        m_file.write_string_to_file(m_formatter(formatted));
    }
}
