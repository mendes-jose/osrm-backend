/*

Copyright (c) 2015, Project OSRM contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef DISTANCE_TABLE_HPP
#define DISTANCE_TABLE_HPP

#include "plugin_base.hpp"

#include "../algorithms/object_encoder.hpp"
#include "../data_structures/query_edge.hpp"
#include "../data_structures/search_engine.hpp"
#include "../descriptors/descriptor_base.hpp"
#include "../util/json_renderer.hpp"
#include "../util/make_unique.hpp"
#include "../util/string_util.hpp"
#include "../util/timing_util.hpp"

#include <osrm/json_container.hpp>

#include <cstdlib>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

template <class DataFacadeT> class DistanceTablePlugin final : public BasePlugin
{
  private:
    std::unique_ptr<SearchEngine<DataFacadeT>> search_engine_ptr;
    int max_locations_distance_table;

  public:
    explicit DistanceTablePlugin(DataFacadeT *facade, const int max_locations_distance_table)
        : max_locations_distance_table(max_locations_distance_table), descriptor_string("table"),
          facade(facade)
    {
        search_engine_ptr = osrm::make_unique<SearchEngine<DataFacadeT>>(facade);
    }

    virtual ~DistanceTablePlugin() {}

    const std::string GetDescriptor() const override final { return descriptor_string; }

    int HandleRequest(const RouteParameters &route_parameters,
                      osrm::json::Object &json_result) override final
    {
        const bool useSameTgtSrc = route_parameters.coordinates.size() ? true : false;
        if ((useSameTgtSrc && route_parameters.destinations.size()) || (useSameTgtSrc && route_parameters.sources.size()))
        {
            return 400;
        }

        if ((useSameTgtSrc && !check_all_coordinates(route_parameters.coordinates)) ||
            (!useSameTgtSrc && !check_all_coordinates(route_parameters.destinations, 2) && !check_all_coordinates(route_parameters.sources, 1)))
        {
            return 400;
        }

        const auto &input_bearings = route_parameters.bearings;
        unsigned nb_coordinates = useSameTgtSrc ? route_parameters.coordinates.size() : (route_parameters.destinations.size() + route_parameters.sources.size());
        if (input_bearings.size() > 0 && nb_coordinates != input_bearings.size())
        {
            json_result.values["status"] = "Number of bearings does not match number of coordinates .";
            return 400;
        }

        const bool checksum_OK = (route_parameters.check_sum == facade->GetCheckSum());
        unsigned max_locations = useSameTgtSrc ?
            std::min(static_cast<unsigned>(max_locations_distance_table),
                     static_cast<unsigned>(route_parameters.coordinates.size())) :
            std::min(static_cast<unsigned>(max_locations_distance_table),
                     static_cast<unsigned>(route_parameters.destinations.size()));

        PhantomNodeArray phantom_node_target_vector(max_locations);
        for (const auto i : osrm::irange(0u, max_locations))
        {
            if (checksum_OK && i < route_parameters.hints.size() &&
                !route_parameters.hints[i].empty())
            {
                PhantomNode current_phantom_node;
                ObjectEncoder::DecodeFromBase64(route_parameters.hints[i], current_phantom_node);
                if (current_phantom_node.is_valid(facade->GetNumberOfNodes()))
                {
                    phantom_node_target_vector[i].emplace_back(std::move(current_phantom_node));
                    continue;
                }
            }
            const int bearing = input_bearings.size() > 0 ? input_bearings[i].first : 0;
            const int range = input_bearings.size() > 0 ? (input_bearings[i].second?*input_bearings[i].second:10) : 180;
            facade->IncrementalFindPhantomNodeForCoordinate(useSameTgtSrc ? route_parameters.coordinates[i] : route_parameters.destinations[i],
                                                            phantom_node_target_vector[i], 1, bearing, range);

            BOOST_ASSERT(phantom_node_target_vector[i].front().is_valid(facade->GetNumberOfNodes()));
        }
        unsigned shift_coordinates = (useSameTgtSrc) ? 0 : route_parameters.destinations.size();
        max_locations = 0;
        if (!useSameTgtSrc)
        {
            max_locations = std::min(static_cast<unsigned>(max_locations_distance_table),
                                     static_cast<unsigned>(route_parameters.sources.size()));
        }
        PhantomNodeArray phantom_node_source_vector(max_locations);
        for (const auto i : osrm::irange(0u, max_locations))
        {
            if (checksum_OK && i < route_parameters.hints.size() - shift_coordinates &&
                !route_parameters.hints[i + shift_coordinates].empty())
            {
                PhantomNode current_phantom_node;
                ObjectEncoder::DecodeFromBase64(route_parameters.hints[i + shift_coordinates], current_phantom_node);
                if (current_phantom_node.is_valid(facade->GetNumberOfNodes()))
                {
                    phantom_node_source_vector[i].emplace_back(std::move(current_phantom_node));
                    continue;
                }
            }
            const int bearing = input_bearings.size() > 0 ? input_bearings[i + shift_coordinates].first : 0;
            const int range = input_bearings.size() > 0 ? (input_bearings[i + shift_coordinates].second?*input_bearings[i + shift_coordinates].second:10) : 180;
            facade->IncrementalFindPhantomNodeForCoordinate(route_parameters.sources[i],
                                                            phantom_node_source_vector[i], 1, bearing, range);

            BOOST_ASSERT(phantom_node_source_vector[i].front().is_valid(facade->GetNumberOfNodes()));
        }

        // TIMER_START(distance_table);
        std::shared_ptr<std::vector<EdgeWeight>> result_table =
            search_engine_ptr->distance_table(phantom_node_target_vector, phantom_node_source_vector);
        // TIMER_STOP(distance_table);

        if (!result_table)
        {
            return 400;
        }

        osrm::json::Array matrix_json_array;
        const auto number_of_targets = phantom_node_target_vector.size();
        const auto number_of_sources = (phantom_node_source_vector.size()) ? phantom_node_source_vector.size() : number_of_targets;
        for (const auto row : osrm::irange<std::size_t>(0, number_of_sources))
        {
            osrm::json::Array json_row;
            auto row_begin_iterator = result_table->begin() + (row * number_of_targets);
            auto row_end_iterator = result_table->begin() + ((row + 1) * number_of_targets);
            json_row.values.insert(json_row.values.end(), row_begin_iterator, row_end_iterator);
            matrix_json_array.values.push_back(json_row);
        }
        json_result.values["distance_table"] = matrix_json_array;
        if (route_parameters.mapped_points) {
            osrm::json::Array target_coord_json_array;
            for (const std::vector<PhantomNode> &phantom_node_vector : phantom_node_target_vector)
            {
                osrm::json::Array json_coord;
                FixedPointCoordinate coord = phantom_node_vector[0].location;
                json_coord.values.push_back(coord.lat / COORDINATE_PRECISION);
                json_coord.values.push_back(coord.lon / COORDINATE_PRECISION);
                target_coord_json_array.values.push_back(json_coord);
            }
            json_result.values["target_mapped_coordinates"] = target_coord_json_array;
            osrm::json::Array source_coord_json_array;
            for (const std::vector<PhantomNode> &phantom_node_vector : phantom_node_source_vector)
            {
                osrm::json::Array json_coord;
                FixedPointCoordinate coord = phantom_node_vector[0].location;
                json_coord.values.push_back(coord.lat / COORDINATE_PRECISION);
                json_coord.values.push_back(coord.lon / COORDINATE_PRECISION);
                source_coord_json_array.values.push_back(json_coord);
            }
            if (source_coord_json_array.values.size())
                json_result.values["source_mapped_coordinates"] = source_coord_json_array;
        }
        // osrm::json::render(reply.content, json_object);
        return 200;
    }

  private:
    std::string descriptor_string;
    DataFacadeT *facade;
};

#endif // DISTANCE_TABLE_HPP
