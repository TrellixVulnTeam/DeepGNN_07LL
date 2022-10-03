// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "src/cc/lib/distributed/server.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <thread>

#include "absl/container/flat_hash_set.h"
#include <glog/logging.h>
#include <glog/raw_logging.h>

#include "src/cc/lib/graph/locator.h"

namespace
{
static const std::string neighbors_prefix = "neighbors_";
static const size_t neighbors_prefix_len = neighbors_prefix.size();

} // namespace

namespace snark
{

GraphEngineServiceImpl::GraphEngineServiceImpl(std::string path, std::vector<uint32_t> partitions,
                                               PartitionStorageType storage_type, std::string config_path,
                                               bool enable_threadpool)
    : m_metadata(path, config_path)
{
    if (enable_threadpool)
    {
        auto concurrency = std::thread::hardware_concurrency();
        m_threadPool = std::make_shared<boost::asio::thread_pool>(concurrency);
    }

    std::vector<std::string> suffixes;
    absl::flat_hash_set<uint32_t> partition_set(std::begin(partitions), std::end(partitions));
    // Go through the path folder with graph binary files.
    // For data generation flexibility we are going to load all files
    // starting with the [file_type(feat/nbs)]_[partition][anything else]
    if (!is_hdfs_path(path))
    {
        for (auto &p : std::filesystem::directory_iterator(path))
        {
            auto full = p.path().stem().string();

            // Use files with neighbor lists to detect eligible suffixes.
            if (full.starts_with(neighbors_prefix) &&
                partition_set.contains(std::stoi(full.substr(neighbors_prefix_len))))
            {
                suffixes.push_back(full.substr(neighbors_prefix_len));
            }
        }
    }
    else
    {
        auto filenames = hdfs_list_directory(path, m_metadata.m_config_path);
        for (auto &full : filenames)
        {
            // Use files with neighbor lists to detect eligible suffixes.
            auto loc = full.find(neighbors_prefix);
            if (loc != std::string::npos && partition_set.contains(std::stoi(full.substr(loc + neighbors_prefix_len))))
            {
                std::filesystem::path full_path = full.substr(loc + neighbors_prefix_len);
                suffixes.push_back(full_path.stem().string());
            }
        }
    }
    std::sort(std::begin(suffixes), std::end(suffixes));
    m_partitions.reserve(suffixes.size());
    for (size_t i = 0; i < suffixes.size(); ++i)
    {
        m_partitions.emplace_back(path, suffixes[i], storage_type);
        ReadNodeMap(path, suffixes[i], i);
    }
}

grpc::Status GraphEngineServiceImpl::GetNodeTypes(::grpc::ServerContext *context,
                                                  const snark::NodeTypesRequest *request,
                                                  snark::NodeTypesReply *response)
{
    for (int curr_offset = 0; curr_offset < request->node_ids().size(); ++curr_offset)
    {
        auto elem = m_node_map.find(request->node_ids()[curr_offset]);
        if (elem == std::end(m_node_map))
        {
            continue;
        }

        auto index = elem->second;
        const size_t partition_count = m_counts[index];
        Type result = snark::DEFAULT_NODE_TYPE;
        for (size_t partition = 0; partition < partition_count && result == snark::DEFAULT_NODE_TYPE;
             ++partition, ++index)
        {
            result = m_partitions[m_partitions_indices[index]].GetNodeType(m_internal_indices[index]);
        }
        if (result == snark::DEFAULT_NODE_TYPE)
            continue;
        response->add_offsets(curr_offset);
        response->add_types(result);
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetNodeFeatures(::grpc::ServerContext *context,
                                                     const snark::NodeFeaturesRequest *request,
                                                     snark::NodeFeaturesReply *response)
{
    std::vector<snark::FeatureMeta> features;
    size_t fv_size = 0;
    for (const auto &feature : request->features())
    {
        features.emplace_back(feature.id(), feature.size());
        fv_size += feature.size();
    }

    // callback function to calculate a portion of the nodes in the full node_ids and then
    // process these nodes.
    auto func = [this, request, fv_size, &features](const std::size_t &start_node_id, const std::size_t &end_node_id,
                                                    std::vector<int> &sub_offset, std::vector<uint8_t> &sub_data) {
        size_t feature_offset = 0;
        for (std::size_t node_offset = start_node_id; node_offset < end_node_id; ++node_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[node_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            for (size_t partition = 0; partition < partition_count; ++partition, ++index)
            {
                if (m_partitions[m_partitions_indices[index]].HasNodeFeatures(m_internal_indices[index]))
                {
                    sub_data.resize(feature_offset + fv_size);
                    auto data = reinterpret_cast<uint8_t *>(sub_data.data());
                    auto data_span = std::span(data + feature_offset, fv_size);

                    m_partitions[m_partitions_indices[index]].GetNodeFeature(m_internal_indices[index], features,
                                                                             data_span);
                    sub_offset.push_back(node_offset);
                    feature_offset += fv_size;
                    break;
                }
            }
        }
    };

    // sub_data structure:
    //  - job index
    //      - feature values
    std::vector<std::vector<int>> sub_offset;
    std::vector<std::vector<uint8_t>> sub_data;

    if (!m_threadPool)
    {
        sub_offset.resize(1);
        sub_data.resize(1);
        func(0, request->node_ids().size(), sub_offset[0], sub_data[0]);
    }
    else
    {
        RunParallel(
            request->node_ids().size(),
            [&sub_offset, &sub_data](const std::size_t &concurrency) {
                sub_offset.resize(concurrency);
                sub_data.resize(concurrency);
            },
            [&sub_offset, &sub_data, func](const std::size_t &index, const std::size_t &start_node_id,
                                           const std::size_t &end_node_id) {
                func(start_node_id, end_node_id, sub_offset[index], sub_data[index]);
            });
    }

    assert(sub_offset.size() == sub_data.size());
    for (std::size_t i = 0; i < sub_offset.size(); i++)
    {
        for (std::size_t k = 0; k < sub_offset[i].size(); k++)
        {
            response->add_offsets(sub_offset[i][k]);
        }

        response->mutable_feature_values()->insert(response->mutable_feature_values()->end(), sub_data[i].begin(),
                                                   sub_data[i].end());
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetEdgeFeatures(::grpc::ServerContext *context,
                                                     const snark::EdgeFeaturesRequest *request,
                                                     snark::EdgeFeaturesReply *response)
{
    const size_t len = request->types().size();

    // First part is source, second is destination
    assert(2 * len == size_t(request->node_ids().size()));
    std::vector<snark::FeatureMeta> features;
    size_t fv_size = 0;
    for (const auto &feature : request->features())
    {
        features.emplace_back(feature.id(), feature.size());
        fv_size += feature.size();
    }

    // callback function to calculate a part of the input_edge_src list.
    auto func = [this, request, fv_size, &features, len](const std::size_t &start_node_id,
                                                         const std::size_t &end_node_id, std::vector<int> &sub_offset,
                                                         std::vector<uint8_t> &sub_data) {
        size_t feature_offset = 0;
        for (size_t node_offset = start_node_id; node_offset < end_node_id; ++node_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[node_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            sub_data.resize(feature_offset + fv_size);
            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            auto data = reinterpret_cast<uint8_t *>(sub_data.data());
            bool found_edge = false;
            for (size_t partition = 0; partition < partition_count && !found_edge; ++partition, ++index)
            {
                found_edge = m_partitions[m_partitions_indices[index]].GetEdgeFeature(
                    m_internal_indices[index], request->node_ids()[len + node_offset], request->types()[node_offset],
                    features, std::span(data + feature_offset, fv_size));
            }
            if (found_edge)
            {
                sub_offset.push_back(node_offset);
                feature_offset += fv_size;
            }
            else
            {
                sub_data.resize(feature_offset);
            }
        }
    };

    std::vector<std::vector<int>> sub_offset;
    std::vector<std::vector<uint8_t>> sub_data;

    if (!m_threadPool)
    {
        sub_offset.resize(1);
        sub_data.resize(1);
        func(0, len, sub_offset[0], sub_data[0]);
    }
    else
    {
        RunParallel(
            len,
            [&sub_offset, &sub_data](const std::size_t &concurrency) {
                sub_offset.resize(concurrency);
                sub_data.resize(concurrency);
            },
            [&sub_offset, &sub_data, func](const std::size_t &index, const std::size_t &start_node_id,
                                           const std::size_t &end_node_id) {
                func(start_node_id, end_node_id, sub_offset[index], sub_data[index]);
            });
    }

    assert(sub_offset.size() == sub_data.size());
    for (std::size_t i = 0; i < sub_offset.size(); i++)
    {
        for (std::size_t k = 0; k < sub_offset[i].size(); k++)
        {
            response->add_offsets(sub_offset[i][k]);
        }

        response->mutable_feature_values()->insert(response->mutable_feature_values()->end(), sub_data[i].begin(),
                                                   sub_data[i].end());
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetNodeSparseFeatures(::grpc::ServerContext *context,
                                                           const snark::NodeSparseFeaturesRequest *request,
                                                           snark::SparseFeaturesReply *response)
{
    std::span<const snark::FeatureId> features =
        std::span(std::begin(request->feature_ids()), std::end(request->feature_ids()));
    auto *reply_dimensions = response->mutable_dimensions();
    reply_dimensions->Resize(int(features.size()), 0);
    auto dimensions = std::span(reply_dimensions->begin(), reply_dimensions->end());
    std::vector<std::vector<std::vector<int64_t>>> indices;
    std::vector<std::vector<std::vector<uint8_t>>> values;

    // callback function to calculate part of the full node list.
    // sub_out_indices & sub_out_data is used to get the results for a specific part,
    // after all parallel job finishes, these sub_out_indices & sub_out_data will be combined.
    auto func = [this, request, &features, &dimensions](
                    const std::size_t &start_node_id, const std::size_t &end_node_id,
                    std::vector<std::vector<int64_t>> &sub_indices, std::vector<std::vector<uint8_t>> &sub_values) {
        for (std::size_t node_offset = start_node_id; node_offset < end_node_id; ++node_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[node_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            bool found = false;
            for (size_t partition = 0; partition < partition_count && !found; ++partition, ++index)
            {
                found = m_partitions[m_partitions_indices[index]].GetNodeSparseFeature(
                    m_internal_indices[index], features, int64_t(node_offset), dimensions, sub_indices, sub_values);
            }
        }
    };

    if (!m_threadPool)
    {
        indices.resize(1);
        values.resize(1);
        indices[0].resize(features.size());
        values[0].resize(features.size());
        func(0, request->node_ids().size(), indices[0], values[0]);
    }
    else
    {
        RunParallel(
            request->node_ids().size(),
            [&values, &indices](const std::size_t &concurrency) {
                indices.resize(concurrency);
                values.resize(concurrency);
            },
            [&indices, &values, func, &features](const std::size_t &index, const std::size_t &start_node_id,
                                                 const std::size_t &end_node_id) {
                indices[index].resize(features.size());
                values[index].resize(features.size());
                func(start_node_id, end_node_id, indices[index], values[index]);
            });
    }

    for (size_t i = 0; i < features.size(); ++i)
    {
        std::size_t indices_sum = 0;
        std::size_t values_sum = 0;
        for (size_t k = 0; k < indices.size(); ++k)
        {
            response->mutable_indices()->Add(std::begin(indices[k][i]), std::end(indices[k][i]));
            response->mutable_values()->append(std::begin(values[k][i]), std::end(values[k][i]));
            indices_sum += indices[k][i].size();
            values_sum += values[k][i].size();
        }

        response->mutable_indices_counts()->Add(indices_sum);
        response->mutable_values_counts()->Add(values_sum);
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetEdgeSparseFeatures(::grpc::ServerContext *context,
                                                           const snark::EdgeSparseFeaturesRequest *request,
                                                           snark::SparseFeaturesReply *response)
{
    const size_t len = request->types().size();

    // First part is source, second is destination
    assert(2 * len == size_t(request->node_ids().size()));
    std::span<const snark::FeatureId> features =
        std::span(std::begin(request->feature_ids()), std::end(request->feature_ids()));
    auto *reply_dimensions = response->mutable_dimensions();
    reply_dimensions->Resize(int(features.size()), 0);
    auto dimensions = std::span(reply_dimensions->begin(), reply_dimensions->end());

    std::vector<std::vector<std::vector<int64_t>>> indices;
    std::vector<std::vector<std::vector<uint8_t>>> values;

    // callback function to calculate a part of the full input_edge_src list.
    auto func = [this, request, &dimensions, len, &features](
                    const std::size_t &start_node_id, const std::size_t &end_node_id,
                    std::vector<std::vector<int64_t>> &sub_indices, std::vector<std::vector<uint8_t>> &sub_values) {
        for (size_t node_offset = start_node_id; node_offset < end_node_id; ++node_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[node_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            bool found_edge = false;
            for (size_t partition = 0; partition < partition_count && !found_edge; ++partition, ++index)
            {
                found_edge = m_partitions[m_partitions_indices[index]].GetEdgeSparseFeature(
                    m_internal_indices[index], request->node_ids()[len + node_offset], request->types()[node_offset],
                    features, int64_t(node_offset), dimensions, sub_indices, sub_values);
            }
        }
    };

    if (!m_threadPool)
    {
        indices.resize(1);
        values.resize(1);
        indices[0].resize(features.size());
        values[0].resize(features.size());
        func(0, len, indices[0], values[0]);
    }
    else
    {
        RunParallel(
            len,
            [&values, &indices](const std::size_t &concurrency) {
                indices.resize(concurrency);
                values.resize(concurrency);
            },
            [&indices, &values, func, &features](const std::size_t &index, const std::size_t &start_node_id,
                                                 const std::size_t &end_node_id) {
                indices[index].resize(features.size());
                values[index].resize(features.size());
                func(start_node_id, end_node_id, indices[index], values[index]);
            });
    }

    for (size_t k = 0; k < indices.size(); ++k)
    {
        for (size_t i = 0; i < features.size(); ++i)
        {
            response->mutable_indices()->Add(std::begin(indices[k][i]), std::end(indices[k][i]));
            response->mutable_values()->append(std::begin(values[k][i]), std::end(values[k][i]));
            response->mutable_indices_counts()->Add(indices[k][i].size());
            response->mutable_values_counts()->Add(values[k][i].size());
        }
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetNodeStringFeatures(::grpc::ServerContext *context,
                                                           const snark::NodeSparseFeaturesRequest *request,
                                                           snark::StringFeaturesReply *response)
{
    std::span<const snark::FeatureId> features =
        std::span(std::begin(request->feature_ids()), std::end(request->feature_ids()));
    const auto features_size = features.size();
    const auto nodes_size = request->node_ids().size();
    auto *reply_dimensions = response->mutable_dimensions();

    reply_dimensions->Resize(int(features_size * nodes_size), 0);
    auto dimensions = std::span(reply_dimensions->begin(), reply_dimensions->end());
    std::vector<std::vector<uint8_t>> sub_values;

    // callback function to calculate part of the full node list.
    auto func = [this, request, &dimensions, features_size, &features](const std::size_t &start_node_id,
                                                                       const std::size_t &end_node_id,
                                                                       std::vector<uint8_t> &sub_values) {
        for (std::size_t node_offset = start_node_id; node_offset < end_node_id; ++node_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[node_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            auto dims_span = dimensions.subspan(features_size * node_offset, features_size);

            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            bool found = false;
            for (size_t partition = 0; partition < partition_count && !found; ++partition, ++index)
            {
                found = m_partitions[m_partitions_indices[index]].GetNodeStringFeature(m_internal_indices[index],
                                                                                       features, dims_span, sub_values);
            }
        }
    };

    if (!m_threadPool)
    {
        sub_values.resize(1);
        func(0, request->node_ids().size(), sub_values[0]);
    }
    else
    {
        RunParallel(
            request->node_ids().size(),
            [&sub_values](const std::size_t &concurrency) { sub_values.resize(concurrency); },
            [&sub_values, func](const std::size_t &index, const std::size_t &start_node_id,
                                const std::size_t &end_node_id) {
                func(start_node_id, end_node_id, sub_values[index]);
            });
    }

    for (size_t k = 0; k < sub_values.size(); ++k)
    {
        response->mutable_values()->append(std::begin(sub_values[k]), std::end(sub_values[k]));
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetEdgeStringFeatures(::grpc::ServerContext *context,
                                                           const snark::EdgeSparseFeaturesRequest *request,
                                                           snark::StringFeaturesReply *response)
{
    const size_t len = request->types().size();

    // First part is source, second is destination
    assert(2 * len == size_t(request->node_ids().size()));
    std::span<const snark::FeatureId> features =
        std::span(std::begin(request->feature_ids()), std::end(request->feature_ids()));
    const auto features_size = features.size();
    auto *reply_dimensions = response->mutable_dimensions();
    reply_dimensions->Resize(int(features_size * len), 0);
    auto dimensions = std::span(reply_dimensions->begin(), reply_dimensions->end());
    std::vector<std::vector<uint8_t>> values;

    // callback function to calculate a part of the full input_edge_src list.
    auto func = [this, &features, request, &dimensions, features_size, len](const std::size_t &start_node_id,
                                                                            const std::size_t &end_node_id,
                                                                            std::vector<uint8_t> &sub_values) {
        for (size_t edge_offset = start_node_id; edge_offset < end_node_id; ++edge_offset)
        {
            auto internal_id = m_node_map.find(request->node_ids()[edge_offset]);
            if (internal_id == std::end(m_node_map))
            {
                continue;
            }

            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            bool found_edge = false;
            for (size_t partition = 0; partition < partition_count && !found_edge; ++partition, ++index)
            {
                found_edge = m_partitions[m_partitions_indices[index]].GetEdgeStringFeature(
                    m_internal_indices[index], request->node_ids()[len + edge_offset], request->types()[edge_offset],
                    features, dimensions.subspan(features_size * edge_offset, features_size), sub_values);
            }
        }
    };

    if (!m_threadPool)
    {
        values.resize(1);
        func(0, len, values[0]);
    }
    else
    {
        RunParallel(
            len, [&values](const std::size_t &concurrency) { values.resize(concurrency); },
            [&values, func](const std::size_t &index, const std::size_t &start_node_id,
                            const std::size_t &end_node_id) { func(start_node_id, end_node_id, values[index]); });
    }

    for (size_t k = 0; k < values.size(); ++k)
    {
        response->mutable_values()->append(std::begin(values[k]), std::end(values[k]));
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetNeighborCounts(::grpc::ServerContext *context,
                                                       const snark::GetNeighborsRequest *request,
                                                       snark::GetNeighborCountsReply *response)
{
    const auto node_count = request->node_ids().size();
    response->mutable_neighbor_counts()->Resize(node_count, 0);
    auto input_edge_types = std::span(std::begin(request->edge_types()), std::end(request->edge_types()));

    for (int node_index = 0; node_index < node_count; ++node_index)
    {
        auto internal_id = m_node_map.find(request->node_ids()[node_index]);
        if (internal_id == std::end(m_node_map))
        {
            continue;
        }
        else
        {
            auto index = internal_id->second;
            size_t partition_count = m_counts[index];
            for (size_t partition = 0; partition < partition_count; ++partition, ++index)
            {
                response->mutable_neighbor_counts()->at(node_index) +=
                    m_partitions[m_partitions_indices[index]].NeighborCount(m_internal_indices[index],
                                                                            input_edge_types);
            }
        }
    }

    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetNeighbors(::grpc::ServerContext *context,
                                                  const snark::GetNeighborsRequest *request,
                                                  snark::GetNeighborsReply *response)
{
    const auto node_count = request->node_ids().size();
    response->mutable_neighbor_counts()->Resize(node_count, 0);
    auto input_edge_types = std::span(std::begin(request->edge_types()), std::end(request->edge_types()));
    std::vector<NodeId> output_neighbor_ids;
    std::vector<Type> output_neighbor_types;
    std::vector<float> output_neighbors_weights;
    for (int node_index = 0; node_index < node_count; ++node_index)
    {
        auto internal_id = m_node_map.find(request->node_ids()[node_index]);
        if (internal_id == std::end(m_node_map))
        {
            continue;
        }
        else
        {
            auto index = internal_id->second;
            const size_t partition_count = m_counts[index];
            for (size_t partition = 0; partition < partition_count; ++partition, ++index)
            {
                response->mutable_neighbor_counts()->at(node_index) +=
                    m_partitions[m_partitions_indices[index]].FullNeighbor(m_internal_indices[index], input_edge_types,
                                                                           output_neighbor_ids, output_neighbor_types,
                                                                           output_neighbors_weights);
                response->mutable_node_ids()->Add(std::begin(output_neighbor_ids), std::end(output_neighbor_ids));
                response->mutable_edge_types()->Add(std::begin(output_neighbor_types), std::end(output_neighbor_types));
                response->mutable_edge_weights()->Add(std::begin(output_neighbors_weights),
                                                      std::end(output_neighbors_weights));
                output_neighbor_ids.resize(0);
                output_neighbor_types.resize(0);
                output_neighbors_weights.resize(0);
            }
        }
    }
    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::WeightedSampleNeighbors(::grpc::ServerContext *context,
                                                             const snark::WeightedSampleNeighborsRequest *request,
                                                             snark::WeightedSampleNeighborsReply *response)
{
    assert(std::is_sorted(std::begin(request->edge_types()), std::end(request->edge_types())));

    size_t count = request->count();
    size_t nodes_found = 0;
    auto input_edge_types = std::span(std::begin(request->edge_types()), std::end(request->edge_types()));
    auto seed = request->seed();

    for (int node_index = 0; node_index < request->node_ids().size(); ++node_index)
    {
        const auto node_id = request->node_ids()[node_index];
        auto internal_id = m_node_map.find(node_id);
        if (internal_id == std::end(m_node_map))
        {
            continue;
        }
        size_t offset = nodes_found * count;
        ++nodes_found;
        const auto index = internal_id->second;
        const size_t partition_count = m_counts[index];
        response->add_node_ids(node_id);
        response->mutable_shard_weights()->Resize(nodes_found, {});
        auto &last_shard_weight = response->mutable_shard_weights()->at(nodes_found - 1);
        response->mutable_neighbor_ids()->Resize(nodes_found * count, request->default_node_id());
        response->mutable_neighbor_types()->Resize(nodes_found * count, request->default_edge_type());
        response->mutable_neighbor_weights()->Resize(nodes_found * count, request->default_node_weight());
        for (size_t partition = 0; partition < partition_count; ++partition)
        {
            m_partitions[m_partitions_indices[index + partition]].SampleNeighbor(
                seed++, m_internal_indices[index + partition], input_edge_types, count,
                std::span(response->mutable_neighbor_ids()->mutable_data() + offset, count),
                std::span(response->mutable_neighbor_types()->mutable_data() + offset, count),
                std::span(response->mutable_neighbor_weights()->mutable_data() + offset, count), last_shard_weight,
                request->default_node_id(), request->default_node_weight(), request->default_edge_type());
        }
    }
    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::UniformSampleNeighbors(::grpc::ServerContext *context,
                                                            const snark::UniformSampleNeighborsRequest *request,
                                                            snark::UniformSampleNeighborsReply *response)
{
    assert(std::is_sorted(std::begin(request->edge_types()), std::end(request->edge_types())));

    size_t count = request->count();
    size_t nodes_found = 0;
    bool without_replacement = request->without_replacement();
    auto input_edge_types = std::span(std::begin(request->edge_types()), std::end(request->edge_types()));
    auto seed = request->seed();

    for (int node_index = 0; node_index < request->node_ids().size(); ++node_index)
    {
        const auto node_id = request->node_ids()[node_index];
        auto internal_id = m_node_map.find(node_id);
        if (internal_id == std::end(m_node_map))
        {
            continue;
        }
        size_t offset = nodes_found * count;
        ++nodes_found;
        const auto index = internal_id->second;
        const size_t partition_count = m_counts[index];
        response->add_node_ids(node_id);
        response->mutable_shard_counts()->Resize(nodes_found, {});
        auto &last_shard_weight = response->mutable_shard_counts()->at(nodes_found - 1);
        response->mutable_neighbor_ids()->Resize(nodes_found * count, request->default_node_id());
        response->mutable_neighbor_types()->Resize(nodes_found * count, request->default_edge_type());
        for (size_t partition = 0; partition < partition_count; ++partition)
        {
            m_partitions[m_partitions_indices[index + partition]].UniformSampleNeighbor(
                without_replacement, seed++, m_internal_indices[index + partition], input_edge_types, count,
                std::span(response->mutable_neighbor_ids()->mutable_data() + offset, count),
                std::span(response->mutable_neighbor_types()->mutable_data() + offset, count), last_shard_weight,
                request->default_node_id(), request->default_edge_type());
        }
    }
    return grpc::Status::OK;
}

grpc::Status GraphEngineServiceImpl::GetMetadata(::grpc::ServerContext *context, const snark::EmptyMessage *request,
                                                 snark::MetadataReply *response)
{
    response->set_version(m_metadata.m_version);
    response->set_nodes(m_metadata.m_node_count);
    response->set_edges(m_metadata.m_edge_count);
    response->set_node_types(m_metadata.m_node_type_count);
    response->set_edge_types(m_metadata.m_edge_type_count);
    response->set_node_features(m_metadata.m_node_feature_count);
    response->set_edge_features(m_metadata.m_edge_feature_count);
    response->set_partitions(m_metadata.m_partition_count);
    for (const auto &partition_weights : m_metadata.m_partition_node_weights)
    {
        for (auto weight : partition_weights)
        {
            response->add_node_partition_weights(weight);
        }
    }
    for (const auto &partition_weights : m_metadata.m_partition_edge_weights)
    {
        for (auto weight : partition_weights)
        {
            response->add_edge_partition_weights(weight);
        }
    }
    *response->mutable_node_count_per_type() = {std::begin(m_metadata.m_node_count_per_type),
                                                std::end(m_metadata.m_node_count_per_type)};
    *response->mutable_edge_count_per_type() = {std::begin(m_metadata.m_edge_count_per_type),
                                                std::end(m_metadata.m_edge_count_per_type)};

    return grpc::Status::OK;
}

void GraphEngineServiceImpl::RunParallel(
    const std::size_t &node_ids_size, std::function<void(const std::size_t &count)> preCallback,
    std::function<void(const std::size_t &index, const std::size_t &start_node_id, const std::size_t &end_node_id)>
        callback) const
{
    auto concurrency = std::thread::hardware_concurrency();
    size_t parallel_count = node_ids_size / concurrency;
    concurrency = (parallel_count == 0) ? 1 : concurrency;

    preCallback(concurrency);

    std::vector<std::shared_ptr<std::promise<void>>> results;
    for (unsigned int i = 0; i < concurrency; ++i)
    {
        auto sub_span_len = parallel_count;
        if (i == (concurrency - 1))
        {
            sub_span_len = node_ids_size - (parallel_count * i);
        }

        auto p = std::make_shared<std::promise<void>>();
        results.push_back(p);
        boost::asio::post(*m_threadPool, [p, callback, i, parallel_count, sub_span_len]() {
            auto start_id = parallel_count * i;
            callback(i, start_id, start_id + sub_span_len);
            p->set_value();
        });
    }

    for (auto &res : results)
    {
        res->get_future().get();
    }
}

void GraphEngineServiceImpl::ReadNodeMap(std::filesystem::path path, std::string suffix, uint32_t index)
{
    std::shared_ptr<BaseStorage<uint8_t>> node_map;
    if (!is_hdfs_path(path))
    {
        node_map = std::make_shared<DiskStorage<uint8_t>>(std::move(path), std::move(suffix), open_node_map);
    }
    else
    {
        auto full_path = path / ("node_" + suffix + ".map");
        node_map = std::make_shared<HDFSStreamStorage<uint8_t>>(full_path.c_str(), m_metadata.m_config_path);
    }
    auto node_map_ptr = node_map->start();
    size_t size = node_map->size() / 20;
    m_node_map.reserve(size);
    m_partitions_indices.reserve(size);
    m_internal_indices.reserve(size);
    m_counts.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        uint64_t pair[2];
        if (node_map->read(pair, 8, 2, node_map_ptr) != 2)
        {
            RAW_LOG_FATAL("Failed to read pair in a node maping");
        }

        auto el = m_node_map.find(pair[0]);
        if (el == std::end(m_node_map))
        {
            m_node_map[pair[0]] = m_internal_indices.size();
            m_internal_indices.emplace_back(pair[1]);
            // TODO: compress vectors below?
            m_partitions_indices.emplace_back(index);
            m_counts.emplace_back(1);
        }
        else
        {
            auto old_offset = el->second;
            auto old_count = m_counts[old_offset];
            m_node_map[pair[0]] = m_internal_indices.size();

            std::copy_n(std::begin(m_internal_indices) + old_offset, old_count, std::back_inserter(m_internal_indices));
            m_internal_indices.emplace_back(pair[1]);

            std::copy_n(std::begin(m_partitions_indices) + old_offset, old_count,
                        std::back_inserter(m_partitions_indices));
            m_partitions_indices.emplace_back(index);

            std::fill_n(std::back_inserter(m_counts), old_count + 1, old_count + 1);
        }

        assert(pair[1] == i);
        Type node_type;
        if (node_map->read(&node_type, sizeof(Type), 1, node_map_ptr) != 1)
        {
            RAW_LOG_FATAL("Failed to read node type in a node maping");
        }
    }
}

} // namespace snark
