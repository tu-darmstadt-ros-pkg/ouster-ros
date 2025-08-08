/**
 * @file bag_converter.cpp
 * @brief ROS2 executable to convert bag files containing lidar/imu packets to processed point clouds
// inspired from here: https://github.com/ouster-lidar/ouster-ros/pull/442
**/

 
#include "ouster_ros/os_ros.h"
// clang-format on

#include <rclcpp/rclcpp.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_cpp/writer.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <rosbag2_cpp/readers/sequential_reader.hpp>
#include <rosbag2_cpp/writers/sequential_writer.hpp>
#include <rosbag2_transport/bag_rewrite.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <std_msgs/msg/string.hpp>

#include "ouster_sensor_msgs/msg/packet_msg.hpp"

#include "imu_packet_handler.h"
#include "lidar_packet_handler.h"
#include "point_cloud_processor.h"
#include "point_cloud_processor_factory.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>

namespace ouster_ros {

namespace sensor = ouster::sensor;
using ouster_sensor_msgs::msg::PacketMsg;

// Helper function to sanitize namespace by removing leading and trailing slashes
std::string sanitize_namespace(const std::string& ns) {
    if (ns.empty()) {
        return "ouster"; // Default namespace if empty
    }
    
    std::string sanitized = ns;
    
    // Remove leading slashes
    while (!sanitized.empty() && sanitized.front() == '/') {
        sanitized.erase(0, 1);
    }
    
    // Remove trailing slashes
    while (!sanitized.empty() && sanitized.back() == '/') {
        sanitized.pop_back();
    }
    
    // If after sanitization it's empty, use default
    if (sanitized.empty()) {
        sanitized = "ouster";
    }
    
    return sanitized;
}

class BagConverter {
public:
    // Added delay_factor parameter to control processing speed and memory usage
    BagConverter(const std::string& input_bag, const std::string& output_bag, 
                 const std::string& ns = "ouster", const std::string& metadata_file = "",
                 int delay_factor = 1)
        : input_bag_(input_bag), output_bag_(output_bag), ns_(sanitize_namespace(ns)), 
          metadata_file_(metadata_file), throttle_factor_(delay_factor) {
        
        // Log namespace sanitization if it was changed
        if (ns != ns_) {
            std::cout << "Namespace sanitized: '" << ns << "' -> '" << ns_ << "'" << std::endl;
        } else {
            std::cout << "Using namespace: '" << ns_ << "'" << std::endl;
        }
    }

    void process() {
        // Initialize ROS2 (needed for time handling)
        rclcpp::init(0, nullptr);
        
        auto reader = std::make_unique<rosbag2_cpp::readers::SequentialReader>();
        auto writer = std::make_unique<rosbag2_cpp::writers::SequentialWriter>();

        // Auto-detect storage format based on bag contents
        std::string storage_format = "sqlite3"; // Default to sqlite3
        
        // Check if the input path contains .mcap files or if it's an .mcap file itself
        std::string bag_path = input_bag_;
        if (bag_path.find(".mcap") != std::string::npos) {
            storage_format = "mcap";
        } else if (std::filesystem::is_directory(bag_path)) {
            // It's a directory, check if it contains .mcap files
            try {
                for (const auto& entry : std::filesystem::directory_iterator(bag_path)) {
                    if (entry.path().extension() == ".mcap") {
                        storage_format = "mcap";
                        break;
                    }
                }
            } catch (const std::exception& e) {
                // If we can't read the directory, fall back to default
                std::cerr << "Warning: Could not read directory " << bag_path << ", using default storage format" << std::endl;
            }
        }
        
        std::cout << "Detected storage format: " << storage_format << std::endl;
        
        // Configure storage options for reading
        rosbag2_storage::StorageOptions read_storage_options;
        read_storage_options.uri = input_bag_;
        read_storage_options.storage_id = storage_format;

        // Configure storage options for writing
        rosbag2_storage::StorageOptions write_storage_options;
        write_storage_options.uri = output_bag_;
        write_storage_options.storage_id = storage_format; // Use same format for output

        rosbag2_cpp::ConverterOptions converter_options;
        converter_options.input_serialization_format = "cdr";
        converter_options.output_serialization_format = "cdr";

        reader->open(read_storage_options, converter_options);
        writer->open(write_storage_options, converter_options);

        // Create topics in the output bag by reading all topic info from input bag first
        auto topics_and_types = reader->get_all_topics_and_types();
        for (const auto& topic_info : topics_and_types) {
            writer->create_topic(topic_info);
        }
        
        // Also create the new topics we'll be adding (point cloud and IMU)
        rosbag2_storage::TopicMetadata pc_topic_info;
        pc_topic_info.name = "/" + ns_ + "/points";
        pc_topic_info.type = "sensor_msgs/msg/PointCloud2";
        pc_topic_info.serialization_format = "cdr";
        writer->create_topic(pc_topic_info);
        std::cout << "Will create output topic: " << pc_topic_info.name << std::endl;
        
        rosbag2_storage::TopicMetadata imu_topic_info;
        imu_topic_info.name = "/" + ns_ + "/imu";
        imu_topic_info.type = "sensor_msgs/msg/Imu";
        imu_topic_info.serialization_format = "cdr";
        writer->create_topic(imu_topic_info);
        std::cout << "Will create output topic: " << imu_topic_info.name << std::endl;

        sensor::sensor_info info;
        rclcpp::Time metadata_ts;

        // Determine the metadata source: file or bag
        if (metadata_file_.empty()) {
            // First pass: find and process metadata in the bag
            bool metadata_found = false;
            std::string metadata_topic = "/" + ns_ + "/metadata";
            std::cout << "Searching for metadata topic: " << metadata_topic << std::endl;
            
            while (reader->has_next()) {
                auto bag_message = reader->read_next();
                if (bag_message->topic_name == metadata_topic) {
                    auto metadata_msg = std::make_shared<std_msgs::msg::String>();
                    rclcpp::Serialization<std_msgs::msg::String> serialization;
                    rclcpp::SerializedMessage serialized_msg(*bag_message->serialized_data);
                    serialization.deserialize_message(&serialized_msg, metadata_msg.get());
                    
                    metadata_ts = rclcpp::Time(bag_message->recv_timestamp, RCL_ROS_TIME);
                    info = sensor::parse_metadata(metadata_msg->data);
                    metadata_found = true;
                    std::cout << "Found metadata in bag file" << std::endl;
                    break;
                }
            }

            if (!metadata_found) {
                std::cerr << "No metadata found in bag file" << std::endl;
                return;
            }

            // Reset reader to beginning
            reader.reset();
            reader = std::make_unique<rosbag2_cpp::readers::SequentialReader>();
            reader->open(read_storage_options, converter_options);
        } else {
            // Read metadata from the specified file
            std::ifstream file(metadata_file_);
            if (!file.is_open()) {
                std::cerr << "Could not open metadata file: " << metadata_file_ << std::endl;
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            std::string metadata_string = buffer.str();
            std::cout << "Loaded metadata from file: " << metadata_file_ << std::endl;
            // When metadata is loaded from file, set metadata_ts to zero so that all pointcloud messages are valid.
            metadata_ts = rclcpp::Time(0, 0, RCL_ROS_TIME);
            info = sensor::parse_metadata(metadata_string);
        }

        std::mutex write_mutex;
        rclcpp::Time lidar_packet_time;

        // Pre-allocate reusable serialization objects to reduce memory allocations
        rclcpp::Serialization<sensor_msgs::msg::PointCloud2> pc_serialization;
        rclcpp::Serialization<sensor_msgs::msg::Imu> imu_serialization;
        rclcpp::Serialization<PacketMsg> packet_serialization;
        
        // Add throttling variables
        size_t processed_lidar_packets = 0;
        int sleep_microseconds = throttle_factor_ * 10; // Convert throttle factor to microseconds delay

        std::vector<LidarScanProcessor> lidar_scan_processors;
        lidar_scan_processors.push_back(
            PointCloudProcessorFactory::create_point_cloud_processor(
                "original", info, "os_sensor",
                true, true, true, 0, 10000 * 1000, 1, "",
                [this, &writer, &metadata_ts, &write_mutex, &lidar_packet_time, &pc_serialization](PointCloudProcessor_OutputType msgs) {
                    // For bag conversion, use bag timestamps instead of sensor internal timestamps
                    // to ensure consistent time domain comparison
                    rclcpp::Time msg_time(lidar_packet_time.nanoseconds(), RCL_ROS_TIME);
                    
                    // Debug: Print timestamp comparison info
                    static size_t point_cloud_count = 0;
                    point_cloud_count++;
                    if (point_cloud_count <= 5) { // Only print for first 5 point clouds
                        std::cout << "\nPointCloud " << point_cloud_count << " - msg_time: " << msg_time.nanoseconds() 
                                  << ", metadata_ts: " << metadata_ts.nanoseconds() << std::endl;
                    }
                    
                    if (msg_time < metadata_ts) {
                        if (point_cloud_count <= 5) {
                            std::cout << "Skipping point cloud " << point_cloud_count << " (timestamp before metadata)" << std::endl;
                        }
                        return;
                    }

                    std::lock_guard<std::mutex> lock(write_mutex);
                    
                    if (point_cloud_count <= 5) {
                        std::cout << "Writing point cloud " << point_cloud_count << " to bag" << std::endl;
                    }
                    
                    // Serialize and write the point cloud message
                    auto bag_message = std::make_shared<rosbag2_storage::SerializedBagMessage>();
                    bag_message->topic_name = "/" + ns_ + "/points";
                    bag_message->recv_timestamp = lidar_packet_time.nanoseconds();
                    bag_message->send_timestamp = lidar_packet_time.nanoseconds();
                    
                    rclcpp::SerializedMessage serialized_msg;
                    pc_serialization.serialize_message(msgs[0].get(), &serialized_msg);
                    
                    // Create a deep copy of the serialized data to avoid double-free issues
                    const auto& rcl_msg = serialized_msg.get_rcl_serialized_message();
                    auto serialized_data = std::make_shared<rcutils_uint8_array_t>();
                    *serialized_data = rcutils_get_zero_initialized_uint8_array();
                    
                    // Allocate new memory and copy the data
                    rcutils_allocator_t allocator = rcutils_get_default_allocator();
                    auto ret = rcutils_uint8_array_init(serialized_data.get(), rcl_msg.buffer_length, &allocator);
                    if (ret == RCUTILS_RET_OK) {
                        memcpy(serialized_data->buffer, rcl_msg.buffer, rcl_msg.buffer_length);
                        serialized_data->buffer_length = rcl_msg.buffer_length;
                        bag_message->serialized_data = serialized_data;
                        writer->write(bag_message);
                    } else {
                        std::cerr << "Failed to initialize uint8_array for point cloud" << std::endl;
                    }
                }
            )
        );

        auto lidar_packet_handler = LidarPacketHandler::create(
            info, lidar_scan_processors, "", static_cast<int64_t>(-37.0 * 1e+9), 0.0);

        auto imu_packet_handler = ImuPacketHandler::create(
            info, "os_imu", "", static_cast<int64_t>(-37.0 * 1e+9));

        // Second pass: process all messages, writing original messages and converting lidar/IMU packets.
        size_t count = 0;
        
        // Define topic names for packet processing
        std::string lidar_topic = "/" + ns_ + "/lidar_packets";
        std::string imu_topic = "/" + ns_ + "/imu_packets";
        std::cout << "Will process lidar packets from topic: " << lidar_topic << std::endl;
        std::cout << "Will process IMU packets from topic: " << imu_topic << std::endl;
        
        // Skip counting total messages to avoid extra pass through large bags
        // We'll show progress based on processed messages instead

        while (reader->has_next()) {
            auto bag_message = reader->read_next();
            count++;

            // Write original message to output bag
            {
                std::lock_guard<std::mutex> lock(write_mutex);
                writer->write(bag_message);
            }

            // Process lidar packets
            if (bag_message->topic_name == lidar_topic) {
                processed_lidar_packets++;
                
                // Debug: Print every 1000th lidar packet processing
                if (processed_lidar_packets % 1000 == 0) {
                    std::cout << "\nProcessing lidar packet #" << processed_lidar_packets << std::endl;
                }
                
                auto msg = std::make_shared<PacketMsg>();
                rclcpp::SerializedMessage serialized_msg(*bag_message->serialized_data);
                packet_serialization.deserialize_message(&serialized_msg, msg.get());
                
                if (!msg) {
                    std::cerr << "Received a null lidar packet message" << std::endl;
                    continue;
                }
                
                sensor::LidarPacket lidar_packet(msg->buf.size());
                memcpy(lidar_packet.buf.data(), msg->buf.data(), msg->buf.size());
                lidar_packet.host_timestamp = bag_message->recv_timestamp;

                lidar_packet_time = rclcpp::Time(bag_message->recv_timestamp);
                lidar_packet_handler(lidar_packet);
                
                // Add delay to prevent overwhelming the lidar packet handler
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_microseconds));
            }
            // Process IMU packets
            else {
                if (bag_message->topic_name == imu_topic) {
                    auto msg = std::make_shared<PacketMsg>();
                    rclcpp::SerializedMessage serialized_msg(*bag_message->serialized_data);
                    packet_serialization.deserialize_message(&serialized_msg, msg.get());
                    
                    if (msg) {
                        sensor::ImuPacket imu_packet(msg->buf.size());
                        memcpy(imu_packet.buf.data(), msg->buf.data(), msg->buf.size());
                        imu_packet.host_timestamp = bag_message->recv_timestamp;

                        auto imu_msg = imu_packet_handler(imu_packet);
                        
                        std::lock_guard<std::mutex> lock(write_mutex);
                        
                        // Serialize and write the IMU message
                        auto imu_bag_message = std::make_shared<rosbag2_storage::SerializedBagMessage>();
                        imu_bag_message->topic_name = "/" + ns_ + "/imu";
                        imu_bag_message->recv_timestamp = bag_message->recv_timestamp;
                        imu_bag_message->send_timestamp = bag_message->recv_timestamp;
                        
                        rclcpp::SerializedMessage imu_serialized_msg;
                        imu_serialization.serialize_message(&imu_msg, &imu_serialized_msg);
                        
                        // Create a deep copy of the serialized data to avoid double-free issues
                        const auto& rcl_msg = imu_serialized_msg.get_rcl_serialized_message();
                        auto imu_serialized_data = std::make_shared<rcutils_uint8_array_t>();
                        *imu_serialized_data = rcutils_get_zero_initialized_uint8_array();
                        
                        // Allocate new memory and copy the data
                        rcutils_allocator_t allocator = rcutils_get_default_allocator();
                        auto ret = rcutils_uint8_array_init(imu_serialized_data.get(), rcl_msg.buffer_length, &allocator);
                        if (ret == RCUTILS_RET_OK) {
                            memcpy(imu_serialized_data->buffer, rcl_msg.buffer, rcl_msg.buffer_length);
                            imu_serialized_data->buffer_length = rcl_msg.buffer_length;
                            imu_bag_message->serialized_data = imu_serialized_data;
                            writer->write(imu_bag_message);
                        } else {
                            std::cerr << "Failed to initialize uint8_array for IMU" << std::endl;
                        }
                    }
                }
            }

            // Print progress every 1000 messages to reduce I/O overhead
            if (count % 1000 == 0) {
                std::cout << "\rProcessed " << count << " messages (lidar packets: " << processed_lidar_packets << ", delay: " << sleep_microseconds << "μs per packet)..." << std::flush;
            }
        }

        std::cout << std::endl << "Conversion completed! Processed " << count << " messages." << std::endl;
        
        // Clean up handlers first
        std::cout << "Cleaning up lidar scan processors..." << std::endl;
        lidar_scan_processors.clear();
        
        // Clean up in proper order - close writer/reader before shutting down ROS
        std::cout << "Closing writer..." << std::endl;
        writer.reset();
        std::cout << "Closing reader..." << std::endl;
        reader.reset();
        
        // Give a moment for any pending operations to complete
        std::cout << "Waiting for cleanup to complete..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        std::cout << "Shutting down ROS..." << std::endl;
        rclcpp::shutdown();
        std::cout << "ROS shutdown complete." << std::endl;
    }

private:
    std::string input_bag_;
    std::string output_bag_;
    std::string ns_;
    std::string metadata_file_;
    int throttle_factor_;
};

} // namespace ouster_ros

int main(int argc, char** argv) {
    // Usage: bag_converter <input_bag> <output_bag> <ns> [metadata_file] [delay_factor]
    if (argc < 4 || argc > 6) {
        std::cerr << "Usage: bag_converter <input_bag> <output_bag> <ns> [metadata_file] [delay_factor]" << std::endl;
        std::cerr << "  delay_factor: microseconds delay multiplier per lidar packet (default: 1, higher = slower processing, less memory usage)" << std::endl;
        return 1;
    }

    std::string metadata_file = (argc >= 5) ? argv[4] : "";
    int delay_factor = (argc == 6) ? std::stoi(argv[5]) : 1;
    
    std::cout << "Using delay factor: " << delay_factor << " (" << (delay_factor * 10) << " microseconds delay per lidar packet)" << std::endl;
    
    ouster_ros::BagConverter converter(argv[1], argv[2], argv[3], metadata_file, delay_factor);
    converter.process();

    return 0;
}
