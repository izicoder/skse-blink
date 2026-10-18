#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#define DLLEXPORT extern "C" __declspec(dllexport)
#define UNUSED(x) (void)(x)

using namespace std::literals;
namespace logger = SKSE::log;
