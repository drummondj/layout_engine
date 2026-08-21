#include <iostream>

#include "../temp/root.hpp"

using namespace layout_engine;

int main(int argc, char **argv)
{
    auto root = new Root();

    auto library_id = root->create_library(
        LibraryData{
            .name = "test_library",
        });

    auto design_id = root->create_design(
        DesignData{
            .library = library_id,
            .name = "top_design",
        });

    InstanceId last_id;
    for (int i = 0; i < 1000000; i++)
    {
        last_id = root->create_instance(
            InstanceData{
                .design = design_id,
                .name = "U1",
                .reference = "BUFX1",
                .location = Point(i, -i),
            });
    }

    auto const *design_data = root->get_design(design_id);
    std::cout << "Design: " << design_data->name << std::endl;

    auto const *inst_data = root->get_instance(last_id);
    std::cout << "Instance location: x =  " << inst_data->location.x << ", y = " << inst_data->location.y << std::endl;

    int count = 0;
    for (auto _ : root->get_design_instances(design_id))
    {
        count++;
    }

    std::cout << "Found " << count << " instances in " << design_data->name << std::endl;

    auto const design_id_by_name = root->get_design_by_name("top_design");
    std::cout << "Found top_design id = " << design_id_by_name << std::endl;

    auto const missing_id = root->get_design_by_name("undefined");
    std::cout << "Found missing id = " << missing_id << std::endl;

    return 0;
}
