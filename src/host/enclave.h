// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "ccf/ds/logger.h"
#include "ccf/version.h"
#include "enclave/interface.h"

#include <dlfcn.h>
#include <filesystem>

#ifdef CCHOST_SUPPORTS_SGX
#  include <ccf_u.h>
#  include <openenclave/bits/result.h>
#  include <openenclave/host.h>
#  include <openenclave/trace.h>
#endif

#ifdef CCHOST_SUPPORTS_VIRTUAL
// Include order matters. virtual_enclave.h uses the OE definitions if
// available, else creates its own stubs
#  include "enclave/virtual_enclave.h"
#endif

extern "C"
{
#ifdef CCHOST_SUPPORTS_SGX
  void nop_oe_logger(
    void* context,
    bool is_enclave,
    const struct tm* t,
    long int usecs,
    oe_log_level_t level,
    uint64_t host_thread_id,
    const char* message)
  {}
#endif
}

namespace host
{
  void expect_enclave_file_suffix(
    const std::string& file,
    char const* expected_suffix,
    host::EnclaveType type)
  {
    if (!nonstd::ends_with(file, expected_suffix))
    {
      // Remove possible suffixes to try and get root of filename, to build
      // suggested filename
      auto basename = file;
      for (const char* suffix :
           {".signed", ".debuggable", ".so", ".enclave", ".virtual"})
      {
        if (nonstd::ends_with(basename, suffix))
        {
          basename = basename.substr(0, basename.size() - strlen(suffix));
        }
      }
      const auto suggested = fmt::format("{}{}", basename, expected_suffix);
      throw std::logic_error(fmt::format(
        "Given enclave file '{}' does not have suffix expected for enclave "
        "type "
        "{}. Did you mean '{}'?",
        file,
        nlohmann::json(type).dump(),
        suggested));
    }
  }

  /**
   * Wraps an oe_enclave and associated ECalls. New ECalls should be added as
   * methods which construct an appropriate EGeneric-derived param type and pass
   * it to call.
   */
  class Enclave
  {
  private:
#ifdef CCHOST_SUPPORTS_SGX
    oe_enclave_t* sgx_handle = nullptr;
#endif
#ifdef CCHOST_SUPPORTS_VIRTUAL
    void* virtual_handle = nullptr;
#endif

  public:
    /**
     * Create an uninitialized enclave hosting the given library.
     *
     * @param path Path to signed enclave library file
     * @param type Type of enclave to load, influencing what flags should be
     * passed to OE, or whether to dlload a virtual enclave
     */
    Enclave(const std::string& path, EnclaveType type)
    {
      if (!std::filesystem::exists(path))
      {
        throw std::logic_error(
          fmt::format("No enclave file found at {}", path));
      }

      switch (type)
      {
        case host::EnclaveType::SGX_RELEASE:
        case host::EnclaveType::SGX_DEBUG:
        {
#ifdef CCHOST_SUPPORTS_SGX
          uint32_t oe_flags = 0;
          if (type == host::EnclaveType::SGX_DEBUG)
          {
            expect_enclave_file_suffix(path, ".enclave.so.debuggable", type);
            oe_flags |= OE_ENCLAVE_FLAG_DEBUG;
          }
          else
          {
            expect_enclave_file_suffix(path, ".enclave.so.signed", type);
          }

#  ifndef VERBOSE_LOGGING
          oe_log_set_callback(nullptr, nop_oe_logger);
#  endif

          auto err = oe_create_ccf_enclave(
            path.c_str(),
            OE_ENCLAVE_TYPE_SGX,
            oe_flags,
            nullptr,
            0,
            &sgx_handle);

          if (err != OE_OK)
          {
            throw std::logic_error(
              fmt::format("Could not create enclave: {}", oe_result_str(err)));
          }
#else
          throw std::logic_error(
            "SGX enclaves are not supported in current build");
#endif // CCHOST_SUPPORTS_SGX
          break;
        }

        case host::EnclaveType::VIRTUAL:
        {
#ifdef CCHOST_SUPPORTS_VIRTUAL
          expect_enclave_file_suffix(path, ".virtual.so", type);
          virtual_handle = load_virtual_enclave(path.c_str());
#else
          throw std::logic_error(
            "Virtual enclaves not supported in current build");
#endif // CCHOST_SUPPORTS_VIRTUAL
          break;
        }

        default:
        {
          throw std::logic_error(fmt::format(
            "Unsupported enclave type: {}", nlohmann::json(type).dump()));
        }
      }
    }

    CreateNodeStatus create_node(
      const EnclaveConfig& enclave_config,
      const StartupConfig& ccf_config,
      std::vector<uint8_t>& node_cert,
      std::vector<uint8_t>& service_cert,
      StartType start_type,
      size_t num_worker_thread,
      void* time_location)
    {
      CreateNodeStatus status = CreateNodeStatus::InternalError;
      constexpr size_t enclave_version_size = 256;
      std::vector<uint8_t> enclave_version_buf(enclave_version_size);

      size_t node_cert_len = 0;
      size_t service_cert_len = 0;
      size_t enclave_version_len = 0;

      auto config = nlohmann::json(ccf_config).dump();

      // Pad config with NULLs to a multiple of 8
      const auto padded_size = (config.size() + 7) & ~(7ull);
      if (config.size() != padded_size)
      {
        LOG_INFO_FMT(
          "Padding config with {} additional nulls",
          padded_size - config.size());
        config.resize(padded_size);
      }

#define CREATE_NODE_ARGS \
  &status, (void*)&enclave_config, config.data(), config.size(), \
    node_cert.data(), node_cert.size(), &node_cert_len, service_cert.data(), \
    service_cert.size(), &service_cert_len, enclave_version_buf.data(), \
    enclave_version_buf.size(), &enclave_version_len, start_type, \
    num_worker_thread, time_location

      oe_result_t err = OE_FAILURE;

// Assume that constructor correctly set the appropriate field, and call
// appropriate function
#ifdef CCHOST_SUPPORTS_VIRTUAL
      if (virtual_handle != nullptr)
      {
        err = virtual_create_node(virtual_handle, CREATE_NODE_ARGS);
      }
#endif
#ifdef CCHOST_SUPPORTS_SGX
      if (sgx_handle != nullptr)
      {
        err = enclave_create_node(sgx_handle, CREATE_NODE_ARGS);
      }
#endif

      if (err != OE_OK || status != CreateNodeStatus::OK)
      {
        // Logs have described the errors already, we just need to allow the
        // host to read them (via read_all()).
        return status;
      }

      // Host and enclave versions must match. Otherwise the node may crash much
      // later (e.g. unhandled ring buffer message on either end)
      auto enclave_version = std::string(
        enclave_version_buf.begin(),
        enclave_version_buf.begin() + enclave_version_len);
      if (ccf::ccf_version != enclave_version)
      {
        LOG_FAIL_FMT(
          "Host/Enclave versions mismatch: {} != {}",
          ccf::ccf_version,
          enclave_version);
        return CreateNodeStatus::VersionMismatch;
      }

      node_cert.resize(node_cert_len);
      service_cert.resize(service_cert_len);

      return CreateNodeStatus::OK;
    }

    // Run a processor over this circuit inside the enclave - should be called
    // from a thread
    bool run()
    {
      bool ret = true;
      oe_result_t err = OE_FAILURE;

#ifdef CCHOST_SUPPORTS_VIRTUAL
      if (virtual_handle != nullptr)
      {
        err = virtual_run(virtual_handle, &ret);
      }
#endif
#ifdef CCHOST_SUPPORTS_SGX
      if (sgx_handle != nullptr)
      {
        err = enclave_run(sgx_handle, &ret);
      }
#endif

      if (err != OE_OK)
      {
        throw std::logic_error(
          fmt::format("Failed to call in enclave_run: {}", oe_result_str(err)));
      }

      return ret;
    }
  };
}
