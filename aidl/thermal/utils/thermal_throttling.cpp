/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define ATRACE_TAG (ATRACE_TAG_THERMAL | ATRACE_TAG_HAL)

#include "thermal_throttling.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <utils/Trace.h>

#include <iterator>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "power_files.h"
#include "thermal_info.h"

namespace aidl {
namespace android {
namespace hardware {
namespace thermal {
namespace implementation {
using ::android::base::StringPrintf;

// To find the next PID target state according to the current thermal severity
size_t getTargetStateOfPID(const SensorInfo &sensor_info, const ThrottlingSeverity curr_severity) {
    size_t target_state = 0;

    for (const auto &severity : ::ndk::enum_range<ThrottlingSeverity>()) {
        size_t state = static_cast<size_t>(severity);
        if (std::isnan(sensor_info.throttling_info->s_power[state])) {
            continue;
        }
        target_state = state;
        if (severity > curr_severity) {
            break;
        }
    }
    LOG(VERBOSE) << "PID target state = " << target_state;
    return target_state;
}

void ThermalThrottling::parseProfileProperty(std::string_view sensor_name,
                                             const SensorInfo &sensor_info) {
    if (sensor_info.throttling_info == nullptr) {
        return;
    }

    const std::string profile = ::android::base::GetProperty(
            StringPrintf("vendor.thermal.%s.profile", sensor_name.data()), "");

    if (profile.empty() || sensor_info.throttling_info->profile_map.count(profile)) {
        if (profile != thermal_throttling_status_map_[sensor_name.data()].profile) {
            LOG(INFO) << sensor_name.data() << ": throttling profile change to "
                      << ((profile.empty()) ? "default" : profile);
            thermal_throttling_status_map_[sensor_name.data()].profile = profile;
        }
    } else {
        LOG(ERROR) << sensor_name.data() << ": set profile to default because " << profile
                   << " is invalid";
        thermal_throttling_status_map_[sensor_name.data()].profile = "";
    }
}

void ThermalThrottling::clearThrottlingData(std::string_view sensor_name) {
    if (!thermal_throttling_status_map_.count(sensor_name.data())) {
        return;
    }
    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);

    for (auto &pid_power_budget_pair :
         thermal_throttling_status_map_.at(sensor_name.data()).pid_power_budget_map) {
        pid_power_budget_pair.second = std::numeric_limits<int>::max();
    }

    for (auto &pid_cdev_request_pair :
         thermal_throttling_status_map_.at(sensor_name.data()).pid_cdev_request_map) {
        pid_cdev_request_pair.second = 0;
    }

    for (auto &hardlimit_cdev_request_pair :
         thermal_throttling_status_map_.at(sensor_name.data()).hardlimit_cdev_request_map) {
        hardlimit_cdev_request_pair.second = 0;
    }

    for (auto &throttling_release_pair :
         thermal_throttling_status_map_.at(sensor_name.data()).throttling_release_map) {
        throttling_release_pair.second = 0;
    }

    thermal_throttling_status_map_[sensor_name.data()].prev_err = NAN;
    thermal_throttling_status_map_[sensor_name.data()].i_budget = NAN;
    thermal_throttling_status_map_[sensor_name.data()].prev_target =
            static_cast<size_t>(ThrottlingSeverity::NONE);
    thermal_throttling_status_map_[sensor_name.data()].prev_power_budget = NAN;
    thermal_throttling_status_map_[sensor_name.data()].tran_cycle = 0;

    return;
}

bool ThermalThrottling::registerThermalThrottling(
        std::string_view sensor_name, const std::shared_ptr<ThrottlingInfo> &throttling_info,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map) {
    if (thermal_throttling_status_map_.count(sensor_name.data())) {
        LOG(ERROR) << "Sensor " << sensor_name.data() << " throttling map has been registered";
        return false;
    }

    if (throttling_info == nullptr) {
        LOG(ERROR) << "Sensor " << sensor_name.data() << " has no throttling info";
        return false;
    }

    thermal_throttling_status_map_[sensor_name.data()].prev_err = NAN;
    thermal_throttling_status_map_[sensor_name.data()].i_budget = NAN;
    thermal_throttling_status_map_[sensor_name.data()].prev_target =
            static_cast<size_t>(ThrottlingSeverity::NONE);
    thermal_throttling_status_map_[sensor_name.data()].prev_power_budget = NAN;
    thermal_throttling_status_map_[sensor_name.data()].tran_cycle = 0;
    thermal_throttling_status_map_[sensor_name.data()].profile = "";

    for (auto &binded_cdev_pair : throttling_info->binded_cdev_info_map) {
        if (!cooling_device_info_map.count(binded_cdev_pair.first)) {
            LOG(ERROR) << "Could not find " << sensor_name.data() << "'s binded CDEV "
                       << binded_cdev_pair.first;
            return false;
        }
        // Register PID throttling map
        for (const auto &cdev_weight : binded_cdev_pair.second.cdev_weight_for_pid) {
            if (!std::isnan(cdev_weight)) {
                thermal_throttling_status_map_[sensor_name.data()]
                        .pid_power_budget_map[binded_cdev_pair.first] =
                        std::numeric_limits<int>::max();
                thermal_throttling_status_map_[sensor_name.data()]
                        .pid_cdev_request_map[binded_cdev_pair.first] = 0;
                thermal_throttling_status_map_[sensor_name.data()]
                        .cdev_status_map[binded_cdev_pair.first] = 0;
                cdev_all_request_map_[binded_cdev_pair.first].insert(0);
                break;
            }
        }
        // Register hard limit throttling map
        for (const auto &limit_info : binded_cdev_pair.second.limit_info) {
            if (limit_info > 0) {
                thermal_throttling_status_map_[sensor_name.data()]
                        .hardlimit_cdev_request_map[binded_cdev_pair.first] = 0;
                thermal_throttling_status_map_[sensor_name.data()]
                        .cdev_status_map[binded_cdev_pair.first] = 0;
                cdev_all_request_map_[binded_cdev_pair.first].insert(0);
                break;
            }
        }
        // Register throttling release map if power threshold exists
        if (!binded_cdev_pair.second.power_rail.empty()) {
            for (const auto &power_threshold : binded_cdev_pair.second.power_thresholds) {
                if (!std::isnan(power_threshold)) {
                    thermal_throttling_status_map_[sensor_name.data()]
                            .throttling_release_map[binded_cdev_pair.first] = 0;
                    break;
                }
            }
        }
    }
    return true;
}

// return power budget based on PID algo
float ThermalThrottling::updatePowerBudget(
        const Temperature &temp, const SensorInfo &sensor_info,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map,
        std::chrono::milliseconds time_elapsed_ms, ThrottlingSeverity curr_severity,
        const bool max_throttling, const std::vector<float> &sensor_predictions) {
    float p = 0, d = 0;
    float power_budget = std::numeric_limits<float>::max();
    bool target_changed = false;
    bool is_fully_throttle = true;
    bool is_fully_release = true;
    float budget_transient = 0.0;
    auto &throttling_status = thermal_throttling_status_map_.at(temp.name);
    const auto &profile = throttling_status.profile;
    std::string sensor_name = temp.name;

    if (curr_severity == ThrottlingSeverity::NONE) {
        return power_budget;
    }

    // Go through the binded cdev, check current throttle status
    for (const auto &binded_cdev_info_pair :
         ((sensor_info.throttling_info->profile_map.empty() ||
           !sensor_info.throttling_info->profile_map.contains(profile))
                  ? sensor_info.throttling_info->binded_cdev_info_map
                  : sensor_info.throttling_info->profile_map.at(profile))) {
        if (throttling_status.pid_cdev_request_map.at(binded_cdev_info_pair.first) >
            binded_cdev_info_pair.second.limit_info[static_cast<size_t>(curr_severity)]) {
            is_fully_release = false;
        }
        if (throttling_status.pid_cdev_request_map.at(binded_cdev_info_pair.first) <
            binded_cdev_info_pair.second.cdev_ceiling[static_cast<size_t>(curr_severity)]) {
            is_fully_throttle = false;
        }
    }

    const auto target_state = getTargetStateOfPID(sensor_info, curr_severity);
    if (throttling_status.prev_target != static_cast<size_t>(ThrottlingSeverity::NONE) &&
        target_state != throttling_status.prev_target &&
        sensor_info.throttling_info->tran_cycle > 0) {
        throttling_status.tran_cycle = sensor_info.throttling_info->tran_cycle - 1;
        target_changed = true;
    }
    throttling_status.prev_target = target_state;

    // Compute PID
    float target = sensor_info.hot_thresholds[target_state];
    float err = target - temp.value;

    if (max_throttling && err <= 0) {
        return sensor_info.throttling_info->min_alloc_power[target_state];
    }

    // Calculate P budget
    p = err * (err < 0 ? sensor_info.throttling_info->k_po[target_state]
                       : sensor_info.throttling_info->k_pu[target_state]);

    // Calculate I budget
    if (std::isnan(throttling_status.i_budget)) {
        if (std::isnan(sensor_info.throttling_info->i_default_pct)) {
            throttling_status.i_budget = sensor_info.throttling_info->i_default;
        } else {
            float default_i_budget = 0.0;
            for (const auto &binded_cdev_info_pair :
                 sensor_info.throttling_info->binded_cdev_info_map) {
                int max_cdev_vote;
                const CdevInfo &cdev_info = cooling_device_info_map.at(binded_cdev_info_pair.first);
                max_cdev_vote = getCdevMaxRequest(binded_cdev_info_pair.first, &max_cdev_vote);
                default_i_budget += cdev_info.state2power[max_cdev_vote];
            }
            throttling_status.i_budget =
                    default_i_budget * sensor_info.throttling_info->i_default_pct / 100;
        }
    }

    if (err < sensor_info.throttling_info->i_cutoff[target_state]) {
        if (err < 0 &&
            throttling_status.prev_power_budget >
                    sensor_info.throttling_info->min_alloc_power[target_state] &&
            !is_fully_throttle) {
            throttling_status.i_budget += err * sensor_info.throttling_info->k_io[target_state];
        } else if (err > 0 &&
                   throttling_status.prev_power_budget <
                           sensor_info.throttling_info->max_alloc_power[target_state] &&
                   !is_fully_release) {
            throttling_status.i_budget += err * sensor_info.throttling_info->k_iu[target_state];
        }
    }

    if (fabsf(throttling_status.i_budget) > sensor_info.throttling_info->i_max[target_state]) {
        throttling_status.i_budget = sensor_info.throttling_info->i_max[target_state] *
                                     (throttling_status.i_budget > 0 ? 1 : -1);
    }

    // Calculate D budget
    if (!std::isnan(throttling_status.prev_err) &&
        time_elapsed_ms != std::chrono::milliseconds::zero()) {
        d = sensor_info.throttling_info->k_d[target_state] * (err - throttling_status.prev_err) /
            time_elapsed_ms.count();
    }

    // Compute Compensation
    float compensation = 0;
    if (sensor_info.predictor_info != nullptr &&
        sensor_info.predictor_info->support_pid_compensation) {
        const std::vector<float> &prediction_weights =
                sensor_info.predictor_info->prediction_weights;
        for (size_t i = 0; i < sensor_predictions.size(); ++i) {
            float prediction_err = target - (sensor_predictions[i] * sensor_info.multiplier);
            compensation += prediction_weights[i] * prediction_err;
        }
        // apply weight based on current severity level
        compensation *= sensor_info.predictor_info->k_p_compensate[target_state];
    }

    throttling_status.prev_err = err;
    // Calculate power budget
    power_budget = sensor_info.throttling_info->s_power[target_state] + p +
                   throttling_status.i_budget + d + compensation;

    power_budget =
            std::clamp(power_budget, sensor_info.throttling_info->min_alloc_power[target_state],
                       sensor_info.throttling_info->max_alloc_power[target_state]);

    if (target_changed) {
        throttling_status.budget_transient = throttling_status.prev_power_budget - power_budget;
    }

    if (throttling_status.tran_cycle) {
        budget_transient = throttling_status.budget_transient *
                           ((static_cast<float>(throttling_status.tran_cycle) /
                             static_cast<float>(sensor_info.throttling_info->tran_cycle)));
        power_budget += budget_transient;
        throttling_status.tran_cycle--;
    }

    LOG(INFO) << temp.name << " power_budget=" << power_budget << " err=" << err
              << " s_power=" << sensor_info.throttling_info->s_power[target_state]
              << " time_elapsed_ms=" << time_elapsed_ms.count() << " p=" << p
              << " i=" << throttling_status.i_budget << " d=" << d
              << " compensation=" << compensation << " budget transient=" << budget_transient
              << " control target=" << target_state;

    ATRACE_INT((sensor_name + std::string("-power_budget")).c_str(),
               static_cast<int>(power_budget));
    ATRACE_INT((sensor_name + std::string("-s_power")).c_str(),
               static_cast<int>(sensor_info.throttling_info->s_power[target_state]));
    ATRACE_INT((sensor_name + std::string("-time_elapsed_ms")).c_str(),
               static_cast<int>(time_elapsed_ms.count()));
    ATRACE_INT((sensor_name + std::string("-budget_transient")).c_str(),
               static_cast<int>(budget_transient));
    ATRACE_INT((sensor_name + std::string("-i")).c_str(),
               static_cast<int>(throttling_status.i_budget));
    ATRACE_INT((sensor_name + std::string("-target_state")).c_str(),
               static_cast<int>(target_state));

    ATRACE_INT((sensor_name + std::string("-err")).c_str(),
               static_cast<int>(err / sensor_info.multiplier));
    ATRACE_INT((sensor_name + std::string("-p")).c_str(), static_cast<int>(p));
    ATRACE_INT((sensor_name + std::string("-d")).c_str(), static_cast<int>(d));
    ATRACE_INT((sensor_name + std::string("-predict_compensation")).c_str(),
               static_cast<int>(compensation));
    ATRACE_INT((sensor_name + std::string("-temp")).c_str(),
               static_cast<int>(temp.value / sensor_info.multiplier));

    throttling_status.prev_power_budget = power_budget;

    return power_budget;
}

float ThermalThrottling::computeExcludedPower(
        const SensorInfo &sensor_info, const ThrottlingSeverity curr_severity,
        const std::unordered_map<std::string, PowerStatus> &power_status_map, std::string *log_buf,
        std::string_view sensor_name) {
    float excluded_power = 0.0;

    for (const auto &excluded_power_info_pair :
         sensor_info.throttling_info->excluded_power_info_map) {
        const auto last_updated_avg_power =
                power_status_map.at(excluded_power_info_pair.first).last_updated_avg_power;
        if (!std::isnan(last_updated_avg_power)) {
            excluded_power += last_updated_avg_power *
                              excluded_power_info_pair.second[static_cast<size_t>(curr_severity)];
            log_buf->append(StringPrintf(
                    "(%s: %0.2f mW, cdev_weight: %f)", excluded_power_info_pair.first.c_str(),
                    last_updated_avg_power,
                    excluded_power_info_pair.second[static_cast<size_t>(curr_severity)]));

            ATRACE_INT((std::string(sensor_name) + std::string("-") +
                        excluded_power_info_pair.first + std::string("-avg_power"))
                               .c_str(),
                       static_cast<int>(last_updated_avg_power));
        }
    }

    ATRACE_INT((std::string(sensor_name) + std::string("-excluded_power")).c_str(),
               static_cast<int>(excluded_power));
    return excluded_power;
}

// Allocate power budget to binded cooling devices base on the real ODPM power data
bool ThermalThrottling::allocatePowerToCdev(
        const Temperature &temp, const SensorInfo &sensor_info,
        const ThrottlingSeverity curr_severity, const std::chrono::milliseconds time_elapsed_ms,
        const std::unordered_map<std::string, PowerStatus> &power_status_map,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map,
        const bool max_throttling, const std::vector<float> &sensor_predictions) {
    float total_weight = 0;
    float last_updated_avg_power = NAN;
    float allocated_power = 0;
    float allocated_weight = 0;
    bool low_power_device_check = true;
    bool is_budget_allocated = false;
    bool power_data_invalid = false;
    std::set<std::string> allocated_cdev;
    std::string log_buf;

    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);
    auto total_power_budget =
            updatePowerBudget(temp, sensor_info, cooling_device_info_map, time_elapsed_ms,
                              curr_severity, max_throttling, sensor_predictions);
    const auto &profile = thermal_throttling_status_map_[temp.name].profile;

    if (sensor_info.throttling_info->excluded_power_info_map.size()) {
        total_power_budget -= computeExcludedPower(sensor_info, curr_severity, power_status_map,
                                                   &log_buf, temp.name);
        total_power_budget = std::max(total_power_budget, 0.0f);
        if (!log_buf.empty()) {
            LOG(INFO) << temp.name << " power budget=" << total_power_budget << " after " << log_buf
                      << " is excluded";
        }
    }

    // Go through binded cdev, compute total cdev weight
    for (const auto &binded_cdev_info_pair :
         (sensor_info.throttling_info->profile_map.count(profile)
                  ? sensor_info.throttling_info->profile_map.at(profile)
                  : sensor_info.throttling_info->binded_cdev_info_map)) {
        const auto cdev_weight = binded_cdev_info_pair.second
                                         .cdev_weight_for_pid[static_cast<size_t>(curr_severity)];
        if (!binded_cdev_info_pair.second.enabled) {
            continue;
        } else if (std::isnan(cdev_weight) || cdev_weight == 0) {
            allocated_cdev.insert(binded_cdev_info_pair.first);
            continue;
        }
        total_weight += cdev_weight;
    }

    while (!is_budget_allocated) {
        for (const auto &binded_cdev_info_pair :
             (sensor_info.throttling_info->profile_map.count(profile)
                      ? sensor_info.throttling_info->profile_map.at(profile)
                      : sensor_info.throttling_info->binded_cdev_info_map)) {
            float cdev_power_adjustment = 0;
            const auto cdev_weight =
                    binded_cdev_info_pair.second
                            .cdev_weight_for_pid[static_cast<size_t>(curr_severity)];

            if (allocated_cdev.count(binded_cdev_info_pair.first)) {
                continue;
            }

            // Get the power data
            if (!power_data_invalid) {
                if (!binded_cdev_info_pair.second.power_rail.empty()) {
                    last_updated_avg_power =
                            power_status_map.at(binded_cdev_info_pair.second.power_rail)
                                    .last_updated_avg_power;
                    if (std::isnan(last_updated_avg_power)) {
                        LOG(VERBOSE) << "power data is under collecting";
                        power_data_invalid = true;
                        break;
                    }

                    ATRACE_INT((temp.name + std::string("-") +
                                binded_cdev_info_pair.second.power_rail + std::string("-avg_power"))
                                       .c_str(),
                               static_cast<int>(last_updated_avg_power));
                } else {
                    power_data_invalid = true;
                    break;
                }
                if (binded_cdev_info_pair.second.throttling_with_power_link) {
                    return false;
                }
            }

            auto cdev_power_budget = total_power_budget * (cdev_weight / total_weight);
            cdev_power_adjustment = cdev_power_budget - last_updated_avg_power;

            if (low_power_device_check) {
                // Share the budget for the CDEV which power is lower than target
                if (cdev_power_adjustment > 0 &&
                    thermal_throttling_status_map_[temp.name].pid_cdev_request_map.at(
                            binded_cdev_info_pair.first) == 0) {
                    allocated_power += last_updated_avg_power;
                    allocated_weight += cdev_weight;
                    allocated_cdev.insert(binded_cdev_info_pair.first);
                    if (!binded_cdev_info_pair.second.power_rail.empty()) {
                        log_buf.append(StringPrintf("(%s: %0.2f mW)",
                                                    binded_cdev_info_pair.second.power_rail.c_str(),
                                                    last_updated_avg_power));
                    }
                    LOG(VERBOSE) << temp.name << " binded " << binded_cdev_info_pair.first
                                 << " has been already at min state 0";
                }
            } else {
                const CdevInfo &cdev_info = cooling_device_info_map.at(binded_cdev_info_pair.first);
                if (!binded_cdev_info_pair.second.power_rail.empty()) {
                    log_buf.append(StringPrintf("(%s: %0.2f mW)",
                                                binded_cdev_info_pair.second.power_rail.c_str(),
                                                last_updated_avg_power));
                }
                // Ignore the power distribution if the CDEV has no space to reduce power
                if ((cdev_power_adjustment < 0 &&
                     thermal_throttling_status_map_[temp.name].pid_cdev_request_map.at(
                             binded_cdev_info_pair.first) == cdev_info.max_state)) {
                    LOG(VERBOSE) << temp.name << " binded " << binded_cdev_info_pair.first
                                 << " has been already at max state " << cdev_info.max_state;
                    continue;
                }

                if (!binded_cdev_info_pair.second.enabled) {
                    cdev_power_budget = cdev_info.state2power[0];
                } else if (!power_data_invalid && binded_cdev_info_pair.second.power_rail != "") {
                    auto cdev_curr_power_budget =
                            thermal_throttling_status_map_[temp.name].pid_power_budget_map.at(
                                    binded_cdev_info_pair.first);

                    if (last_updated_avg_power > cdev_curr_power_budget) {
                        cdev_power_budget = cdev_curr_power_budget +=
                                (cdev_power_adjustment *
                                 (cdev_curr_power_budget / last_updated_avg_power));
                    } else {
                        cdev_power_budget = cdev_curr_power_budget += cdev_power_adjustment;
                    }
                } else {
                    cdev_power_budget = total_power_budget * (cdev_weight / total_weight);
                }

                if (!std::isnan(cdev_info.state2power[0]) &&
                    cdev_power_budget > cdev_info.state2power[0]) {
                    cdev_power_budget = cdev_info.state2power[0];
                } else if (cdev_power_budget < 0) {
                    cdev_power_budget = 0;
                }

                int max_cdev_vote;
                if (!getCdevMaxRequest(binded_cdev_info_pair.first, &max_cdev_vote)) {
                    return false;
                }

                const auto curr_cdev_vote =
                        thermal_throttling_status_map_[temp.name].pid_cdev_request_map.at(
                                binded_cdev_info_pair.first);

                if (!max_throttling) {
                    if (binded_cdev_info_pair.second.max_release_step !=
                                std::numeric_limits<int>::max() &&
                        (power_data_invalid || cdev_power_adjustment > 0)) {
                        if (!power_data_invalid && curr_cdev_vote < max_cdev_vote) {
                            cdev_power_budget = cdev_info.state2power[curr_cdev_vote];
                            LOG(VERBOSE) << temp.name << "'s " << binded_cdev_info_pair.first
                                         << " vote: " << curr_cdev_vote
                                         << " is lower than max cdev vote: " << max_cdev_vote;
                        } else {
                            int target_release_step = binded_cdev_info_pair.second.max_release_step;
                            while ((curr_cdev_vote - target_release_step) >
                                           binded_cdev_info_pair.second
                                                   .limit_info[static_cast<size_t>(
                                                           curr_severity)] &&
                                   cdev_info.state2power[curr_cdev_vote - target_release_step] ==
                                           cdev_info.state2power[curr_cdev_vote]) {
                                target_release_step += 1;
                            }
                            const auto target_state =
                                    std::max(curr_cdev_vote - target_release_step, 0);

                            cdev_power_budget = std::min(cdev_power_budget,
                                                         cdev_info.state2power[target_state]);
                        }
                    }

                    if (binded_cdev_info_pair.second.max_throttle_step !=
                                std::numeric_limits<int>::max() &&
                        (power_data_invalid || cdev_power_adjustment < 0)) {
                        int target_throttle_step = binded_cdev_info_pair.second.max_throttle_step;
                        while ((curr_cdev_vote + target_throttle_step) <
                                       binded_cdev_info_pair.second
                                               .cdev_ceiling[static_cast<size_t>(curr_severity)] &&
                               cdev_info.state2power[curr_cdev_vote + target_throttle_step] ==
                                       cdev_info.state2power[curr_cdev_vote]) {
                            target_throttle_step += 1;
                        }
                        const auto target_state =
                                std::min(curr_cdev_vote + target_throttle_step,
                                         binded_cdev_info_pair.second
                                                 .cdev_ceiling[static_cast<size_t>(curr_severity)]);
                        cdev_power_budget =
                                std::max(cdev_power_budget, cdev_info.state2power[target_state]);
                    }
                }

                thermal_throttling_status_map_[temp.name].pid_power_budget_map.at(
                        binded_cdev_info_pair.first) = cdev_power_budget;
                LOG(VERBOSE) << temp.name << " allocate "
                             << thermal_throttling_status_map_[temp.name].pid_power_budget_map.at(
                                        binded_cdev_info_pair.first)
                             << "mW to " << binded_cdev_info_pair.first
                             << "(cdev_weight=" << cdev_weight << ")";
            }
        }

        if (!power_data_invalid) {
            total_power_budget -= allocated_power;
            total_weight -= allocated_weight;
        }
        allocated_power = 0;
        allocated_weight = 0;

        if (low_power_device_check) {
            low_power_device_check = false;
        } else {
            is_budget_allocated = true;
        }
    }
    if (log_buf.size()) {
        LOG(INFO) << temp.name << " binded power rails: " << log_buf;
    }
    return true;
}

void ThermalThrottling::updateCdevRequestByPower(
        std::string sensor_name,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map) {
    size_t i;

    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);
    for (auto &pid_power_budget_pair :
         thermal_throttling_status_map_[sensor_name.data()].pid_power_budget_map) {
        const CdevInfo &cdev_info = cooling_device_info_map.at(pid_power_budget_pair.first);

        for (i = 0; i < cdev_info.state2power.size() - 1; ++i) {
            if (pid_power_budget_pair.second >= cdev_info.state2power[i]) {
                break;
            }
        }
        thermal_throttling_status_map_[sensor_name.data()].pid_cdev_request_map.at(
                pid_power_budget_pair.first) = static_cast<int>(i);
    }

    return;
}

void ThermalThrottling::updateCdevRequestBySeverity(std::string_view sensor_name,
                                                    const SensorInfo &sensor_info,
                                                    ThrottlingSeverity curr_severity) {
    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);
    const auto &profile = thermal_throttling_status_map_[sensor_name.data()].profile;

    for (const auto &binded_cdev_info_pair :
         (sensor_info.throttling_info->profile_map.count(profile)
                  ? sensor_info.throttling_info->profile_map.at(profile)
                  : sensor_info.throttling_info->binded_cdev_info_map)) {
        if (!thermal_throttling_status_map_[sensor_name.data()].hardlimit_cdev_request_map.count(
                    binded_cdev_info_pair.first)) {
            continue;
        }
        thermal_throttling_status_map_[sensor_name.data()].hardlimit_cdev_request_map.at(
                binded_cdev_info_pair.first) =
                (binded_cdev_info_pair.second.enabled)
                        ? binded_cdev_info_pair.second
                                  .limit_info[static_cast<size_t>(curr_severity)]
                        : 0;
        LOG(VERBOSE) << "Hard Limit: Sensor " << sensor_name.data() << " update cdev "
                     << binded_cdev_info_pair.first << " to "
                     << thermal_throttling_status_map_[sensor_name.data()]
                                .hardlimit_cdev_request_map.at(binded_cdev_info_pair.first);
    }
}

bool ThermalThrottling::throttlingReleaseUpdate(
        std::string_view sensor_name,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map,
        const std::unordered_map<std::string, PowerStatus> &power_status_map,
        const ThrottlingSeverity severity, const SensorInfo &sensor_info) {
    ATRACE_CALL();
    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);
    if (!thermal_throttling_status_map_.count(sensor_name.data())) {
        return false;
    }
    auto &thermal_throttling_status = thermal_throttling_status_map_.at(sensor_name.data());
    for (const auto &binded_cdev_info_pair : sensor_info.throttling_info->binded_cdev_info_map) {
        float avg_power = -1;

        if (!thermal_throttling_status.throttling_release_map.count(binded_cdev_info_pair.first) ||
            !power_status_map.count(binded_cdev_info_pair.second.power_rail)) {
            return false;
        }

        const auto max_state = cooling_device_info_map.at(binded_cdev_info_pair.first).max_state;

        auto &release_step =
                thermal_throttling_status.throttling_release_map.at(binded_cdev_info_pair.first);
        avg_power =
                power_status_map.at(binded_cdev_info_pair.second.power_rail).last_updated_avg_power;

        if (std::isnan(avg_power) || avg_power < 0) {
            release_step = binded_cdev_info_pair.second.throttling_with_power_link ? max_state : 0;
            continue;
        }

        bool is_over_budget = true;
        if (!binded_cdev_info_pair.second.high_power_check) {
            if (avg_power <
                binded_cdev_info_pair.second.power_thresholds[static_cast<int>(severity)]) {
                is_over_budget = false;
            }
        } else {
            if (avg_power >
                binded_cdev_info_pair.second.power_thresholds[static_cast<int>(severity)]) {
                is_over_budget = false;
            }
        }
        LOG(INFO) << sensor_name.data() << "'s " << binded_cdev_info_pair.first
                  << " binded power rail " << binded_cdev_info_pair.second.power_rail
                  << ": power threshold = "
                  << binded_cdev_info_pair.second.power_thresholds[static_cast<int>(severity)]
                  << ", avg power = " << avg_power;
        std::string atrace_prefix = ::android::base::StringPrintf(
                "%s-%s", sensor_name.data(), binded_cdev_info_pair.second.power_rail.data());
        ATRACE_INT(
                (atrace_prefix + std::string("-power_threshold")).c_str(),
                static_cast<int>(
                        binded_cdev_info_pair.second.power_thresholds[static_cast<int>(severity)]));
        ATRACE_INT((atrace_prefix + std::string("-avg_power")).c_str(), avg_power);

        switch (binded_cdev_info_pair.second.release_logic) {
            case ReleaseLogic::INCREASE:
                if (!is_over_budget) {
                    if (std::abs(release_step) < static_cast<int>(max_state)) {
                        release_step--;
                    }
                } else {
                    release_step = 0;
                }
                break;
            case ReleaseLogic::DECREASE:
                if (!is_over_budget) {
                    if (release_step < static_cast<int>(max_state)) {
                        release_step++;
                    }
                } else {
                    release_step = 0;
                }
                break;
            case ReleaseLogic::STEPWISE:
                if (!is_over_budget) {
                    if (release_step < static_cast<int>(max_state)) {
                        release_step++;
                    }
                } else {
                    if (std::abs(release_step) < static_cast<int>(max_state)) {
                        release_step--;
                    }
                }
                break;
            case ReleaseLogic::RELEASE_TO_FLOOR:
                release_step = is_over_budget ? 0 : max_state;
                break;
            case ReleaseLogic::NONE:
            default:
                break;
        }
    }
    return true;
}

void ThermalThrottling::thermalThrottlingUpdate(
        const Temperature &temp, const SensorInfo &sensor_info,
        const ThrottlingSeverity curr_severity, const std::chrono::milliseconds time_elapsed_ms,
        const std::unordered_map<std::string, PowerStatus> &power_status_map,
        const std::unordered_map<std::string, CdevInfo> &cooling_device_info_map,
        const bool max_throttling, const std::vector<float> &sensor_predictions) {
    if (!thermal_throttling_status_map_.count(temp.name)) {
        return;
    }

    if (sensor_info.throttling_info->profile_map.size()) {
        parseProfileProperty(temp.name.c_str(), sensor_info);
    }

    if (thermal_throttling_status_map_[temp.name].pid_power_budget_map.size()) {
        if (!allocatePowerToCdev(temp, sensor_info, curr_severity, time_elapsed_ms,
                                 power_status_map, cooling_device_info_map, max_throttling,
                                 sensor_predictions)) {
            LOG(ERROR) << "Sensor " << temp.name << " PID request cdev failed";
            // Clear the CDEV request if the power budget is failed to be allocated
            for (auto &pid_cdev_request_pair :
                 thermal_throttling_status_map_[temp.name].pid_cdev_request_map) {
                pid_cdev_request_pair.second = 0;
            }
        }
        updateCdevRequestByPower(temp.name, cooling_device_info_map);
    }

    if (thermal_throttling_status_map_[temp.name].hardlimit_cdev_request_map.size()) {
        updateCdevRequestBySeverity(temp.name.c_str(), sensor_info, curr_severity);
    }

    if (thermal_throttling_status_map_[temp.name].throttling_release_map.size()) {
        throttlingReleaseUpdate(temp.name.c_str(), cooling_device_info_map, power_status_map,
                                curr_severity, sensor_info);
    }
}

void ThermalThrottling::computeCoolingDevicesRequest(
        std::string_view sensor_name, const SensorInfo &sensor_info,
        const ThrottlingSeverity curr_severity, std::vector<std::string> *cooling_devices_to_update,
        ThermalStatsHelper *thermal_stats_helper) {
    int release_step = 0;
    std::unique_lock<std::shared_mutex> _lock(thermal_throttling_status_map_mutex_);

    if (!thermal_throttling_status_map_.count(sensor_name.data())) {
        return;
    }

    auto &thermal_throttling_status = thermal_throttling_status_map_.at(sensor_name.data());
    const auto &cdev_release_map = thermal_throttling_status.throttling_release_map;

    const auto &profile = thermal_throttling_status_map_[sensor_name.data()].profile;
    const auto &binded_cdev_info_map =
            sensor_info.throttling_info->profile_map.count(profile)
                    ? sensor_info.throttling_info->profile_map.at(profile)
                    : sensor_info.throttling_info->binded_cdev_info_map;

    for (auto &cdev_request_pair : thermal_throttling_status.cdev_status_map) {
        int pid_cdev_request = 0;
        int hardlimit_cdev_request = 0;
        const auto &cdev_name = cdev_request_pair.first;
        const auto &binded_cdev_info = binded_cdev_info_map.at(cdev_name);
        const auto cdev_ceiling = binded_cdev_info.cdev_ceiling[static_cast<size_t>(curr_severity)];
        const auto cdev_floor =
                binded_cdev_info.cdev_floor_with_power_link[static_cast<size_t>(curr_severity)];
        release_step = 0;

        if (thermal_throttling_status.pid_cdev_request_map.count(cdev_name)) {
            pid_cdev_request = thermal_throttling_status.pid_cdev_request_map.at(cdev_name);
        }

        if (thermal_throttling_status.hardlimit_cdev_request_map.count(cdev_name)) {
            hardlimit_cdev_request =
                    thermal_throttling_status.hardlimit_cdev_request_map.at(cdev_name);
        }

        if (cdev_release_map.count(cdev_name)) {
            release_step = cdev_release_map.at(cdev_name);
        }

        LOG(VERBOSE) << sensor_name.data() << " binded cooling device " << cdev_name
                     << "'s pid_request=" << pid_cdev_request
                     << " hardlimit_cdev_request=" << hardlimit_cdev_request
                     << " release_step=" << release_step
                     << " cdev_floor_with_power_link=" << cdev_floor
                     << " cdev_ceiling=" << cdev_ceiling;
        std::string atrace_prefix =
                ::android::base::StringPrintf("%s-%s", sensor_name.data(), cdev_name.data());
        ATRACE_INT((atrace_prefix + std::string("-pid_request")).c_str(), pid_cdev_request);
        ATRACE_INT((atrace_prefix + std::string("-hardlimit_request")).c_str(),
                   hardlimit_cdev_request);
        ATRACE_INT((atrace_prefix + std::string("-release_step")).c_str(), release_step);
        ATRACE_INT((atrace_prefix + std::string("-cdev_floor")).c_str(), cdev_floor);
        ATRACE_INT((atrace_prefix + std::string("-cdev_ceiling")).c_str(), cdev_ceiling);

        auto request_state = std::max(pid_cdev_request, hardlimit_cdev_request);
        if (release_step) {
            if (release_step >= request_state) {
                request_state = 0;
            } else {
                request_state = request_state - release_step;
            }
            // Only check the cdev_floor when release step is non zero
            request_state = std::max(request_state, cdev_floor);
        }
        request_state = std::min(request_state, cdev_ceiling);
        if (cdev_request_pair.second != request_state) {
            ATRACE_INT((atrace_prefix + std::string("-final_request")).c_str(), request_state);
            if (updateCdevMaxRequestAndNotifyIfChange(cdev_name, cdev_request_pair.second,
                                                      request_state)) {
                cooling_devices_to_update->emplace_back(cdev_name);
            }
            cdev_request_pair.second = request_state;
            // Update sensor cdev request time in state
            thermal_stats_helper->updateSensorCdevRequestStats(sensor_name, cdev_name,
                                                               cdev_request_pair.second);
        }
    }
}

bool ThermalThrottling::updateCdevMaxRequestAndNotifyIfChange(std::string_view cdev_name,
                                                              int cur_request, int new_request) {
    std::unique_lock<std::shared_mutex> _lock(cdev_all_request_map_mutex_);
    auto &request_set = cdev_all_request_map_.at(cdev_name.data());
    int cur_max_request = (*request_set.begin());
    // Remove old cdev request and add the new one.
    request_set.erase(request_set.find(cur_request));
    request_set.insert(new_request);
    // Check if there is any change in aggregated max cdev request.
    int new_max_request = (*request_set.begin());
    LOG(VERBOSE) << "For cooling device [" << cdev_name.data()
                 << "] cur_max_request is: " << cur_max_request
                 << " new_max_request is: " << new_max_request;
    return new_max_request != cur_max_request;
}

bool ThermalThrottling::getCdevMaxRequest(std::string_view cdev_name, int *max_state) {
    std::shared_lock<std::shared_mutex> _lock(cdev_all_request_map_mutex_);
    if (!cdev_all_request_map_.count(cdev_name.data())) {
        LOG(ERROR) << "Cooling device [" << cdev_name.data()
                   << "] not present in cooling device request map";
        return false;
    }
    *max_state = *cdev_all_request_map_.at(cdev_name.data()).begin();
    return true;
}

}  // namespace implementation
}  // namespace thermal
}  // namespace hardware
}  // namespace android
}  // namespace aidl
