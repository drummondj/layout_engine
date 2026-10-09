#include "generated/database/schema_version.hpp"
#include <gtest/gtest.h>
#include <json.hpp>

// The generated schema_version.hpp (codegen's descriptor.py) - the
// descriptor a native-format file embeds (plans/NATIVE_FILE_FORMAT_RESEARCH.md §3).

TEST(SchemaVersion, DescriptorIsValidJsonMatchingVersion)
{
    const auto descriptor = nlohmann::json::parse(le::schema_info::kDescriptorJson);
    EXPECT_EQ(descriptor.at("format").get<int>(), 1);
    EXPECT_EQ(descriptor.at("namespace").get<std::string>(), "le");
    EXPECT_EQ(descriptor.at("version").get<std::string>(), le::schema_info::kVersion);
    EXPECT_EQ(le::schema_info::kFingerprint.size(), 16u);
}

TEST(SchemaVersion, DescriptorDescribesPooledClassesAndParents)
{
    const auto descriptor = nlohmann::json::parse(le::schema_info::kDescriptorJson);
    const nlohmann::json *net = nullptr;
    for (const auto &klass : descriptor.at("classes"))
        if (klass.at("name") == "Net")
            net = &klass;
    ASSERT_NE(net, nullptr);
    EXPECT_EQ(net->at("kind"), "pooled");

    const nlohmann::json *schematic = nullptr;
    for (const auto &field : net->at("fields"))
        if (field.at("name") == "schematic")
            schematic = &field;
    ASSERT_NE(schematic, nullptr);
    EXPECT_EQ(schematic->at("kind"), "parent");
    EXPECT_EQ(schematic->at("type"), "Schematic");
    EXPECT_EQ(schematic->at("parent_field"), "nets");
}
