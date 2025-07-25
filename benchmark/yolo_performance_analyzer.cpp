/*
 * YOLO Performance Analyzer
 * Professional comprehensive benchmarking tool with advanced system monitoring
 * Supports image, video, camera, and automated comprehensive testing modes
 */

#include <iostream>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <memory>
#include <chrono>
#include <thread>
#include <fstream>
#include <map>
#include <sys/resource.h>
#include <iomanip>
#include <filesystem>
#include <ctime>

#include <opencv2/opencv.hpp>

// Use existing project headers
#include "det/YOLO11.hpp"
#include "tools/ScopedTimer.hpp"

// System monitoring includes
#include <sys/stat.h>
#include <sstream>
#include <unistd.h>

// Benchmark configuration structure
struct BenchmarkConfig {
    std::string model_type;      // "yolo5", "yolo8", "yolo11", etc.
    std::string task_type;       // "detection", "segmentation", "obb", "pose"
    std::string model_path;
    std::string labels_path;
    bool use_gpu = false;  // Default to CPU
    int thread_count = 1;
    bool quantized = false;
    std::string precision = "fp32";
};

// Performance metrics structure with enhanced system monitoring
struct PerformanceMetrics {
    double load_time_ms = 0.0;
    double preprocess_avg_ms = 0.0;
    double inference_avg_ms = 0.0;
    double postprocess_avg_ms = 0.0;
    double total_avg_ms = 0.0;
    double fps = 0.0;
    double memory_mb = 0.0;
    double map_score = 0.0;
    int frame_count = 0;
    
    // Enhanced system monitoring
    double cpu_usage_percent = 0.0;
    double gpu_usage_percent = 0.0;
    double gpu_memory_used_mb = 0.0;
    double gpu_memory_total_mb = 0.0;
    double system_memory_used_mb = 0.0;
    double latency_avg_ms = 0.0;
    double latency_min_ms = 0.0;
    double latency_max_ms = 0.0;
    std::string environment_type = "CPU";
};

// System monitoring utilities
struct SystemMonitor {
    static double getCPUUsage() {
        static unsigned long long lastTotalUser = 0, lastTotalUserLow = 0, lastTotalSys = 0, lastTotalIdle = 0;
        
        std::ifstream file("/proc/stat");
        std::string line;
        std::getline(file, line);
        
        unsigned long long totalUser, totalUserLow, totalSys, totalIdle, total;
        std::sscanf(line.c_str(), "cpu %llu %llu %llu %llu", &totalUser, &totalUserLow, &totalSys, &totalIdle);
        
        if (lastTotalUser == 0) {
            lastTotalUser = totalUser;
            lastTotalUserLow = totalUserLow;
            lastTotalSys = totalSys;
            lastTotalIdle = totalIdle;
            return 0.0;
        }
        
        total = (totalUser - lastTotalUser) + (totalUserLow - lastTotalUserLow) + (totalSys - lastTotalSys);
        double percent = total;
        total += (totalIdle - lastTotalIdle);
        percent /= total;
        percent *= 100;
        
        lastTotalUser = totalUser;
        lastTotalUserLow = totalUserLow;
        lastTotalSys = totalSys;
        lastTotalIdle = totalIdle;
        
        return percent;
    }
    
    static std::pair<double, double> getGPUUsage() {
        FILE* pipe = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>/dev/null", "r");
        if (!pipe) return {0.0, 0.0};
        
        char buffer[128];
        std::string result = "";
        while (fgets(buffer, sizeof buffer, pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
        
        if (result.empty()) return {0.0, 0.0};
        
        double gpu_util = 0.0, gpu_mem = 0.0;
        std::sscanf(result.c_str(), "%lf, %lf", &gpu_util, &gpu_mem);
        return {gpu_util, gpu_mem};
    }
    
    static double getSystemMemoryUsage() {
        std::ifstream file("/proc/meminfo");
        std::string line;
        unsigned long memTotal = 0, memFree = 0, buffers = 0, cached = 0;
        
        while (std::getline(file, line)) {
            if (line.find("MemTotal:") == 0) {
                std::sscanf(line.c_str(), "MemTotal: %lu kB", &memTotal);
            } else if (line.find("MemFree:") == 0) {
                std::sscanf(line.c_str(), "MemFree: %lu kB", &memFree);
            } else if (line.find("Buffers:") == 0) {
                std::sscanf(line.c_str(), "Buffers: %lu kB", &buffers);
            } else if (line.find("Cached:") == 0) {
                std::sscanf(line.c_str(), "Cached: %lu kB", &cached);
            }
        }
        
        double usedMB = (memTotal - memFree - buffers - cached) / 1024.0;
        return usedMB;
    }
};

// Memory measurement utility
double getCurrentMemoryUsageMB() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024.0;
}

// Detector factory
class DetectorFactory {
public:
    static std::unique_ptr<YOLO11Detector> createDetector(const BenchmarkConfig& config) {
        // Auto-detect quantized models from file path
        bool is_quantized = config.model_path.find("quantized") != std::string::npos;
        
        if (config.model_type == "yolo11" && config.task_type == "detection") {
            if (is_quantized) {
                std::cout << "Note: Testing YOLO11 quantized model (75% smaller size)" << std::endl;
            }
            return std::make_unique<YOLO11Detector>(config.model_path, config.labels_path, config.use_gpu);
        }
        else if (config.model_type == "yolo8" && config.task_type == "detection") {
            if (is_quantized) {
                std::cout << "Note: Testing YOLO8 quantized model (75% smaller size)" << std::endl;
            } else {
                std::cout << "Note: Using YOLO11 detector for YOLO8 model (compatibility mode)" << std::endl;
            }
            return std::make_unique<YOLO11Detector>(config.model_path, config.labels_path, config.use_gpu);
        }
        else if (config.model_type == "yolo11_quantized" && config.task_type == "detection") {
            std::cout << "Note: Testing YOLO11 quantized model (75% smaller size)" << std::endl;
            return std::make_unique<YOLO11Detector>(config.model_path, config.labels_path, config.use_gpu);
        }
        else if (config.model_type == "yolo8_quantized" && config.task_type == "detection") {
            std::cout << "Note: Testing YOLO8 quantized model (75% smaller size)" << std::endl;
            return std::make_unique<YOLO11Detector>(config.model_path, config.labels_path, config.use_gpu);
        }
        else {
            throw std::runtime_error("Unsupported model type: " + config.model_type + " with task: " + config.task_type);
        }
    }
    
    static std::vector<Detection> detect(YOLO11Detector* detector, const BenchmarkConfig& config, const cv::Mat& image) {
        return detector->detect(image);
    }
};

// Enhanced image benchmark
PerformanceMetrics benchmark_image_comprehensive(const BenchmarkConfig& config,
                                               const std::string& image_path,
                                               int iterations = 100) {
    PerformanceMetrics metrics;
    metrics.environment_type = config.use_gpu ? "GPU" : "CPU";
    
    // Measure model loading time
    auto load_start = std::chrono::high_resolution_clock::now();
    auto detector = DetectorFactory::createDetector(config);
    auto load_end = std::chrono::high_resolution_clock::now();
    metrics.load_time_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();
    
    // Load test image
    cv::Mat image = cv::imread(image_path);
    if (image.empty()) {
        throw std::runtime_error("Could not read image: " + image_path);
    }
    
    std::vector<double> preprocess_times, inference_times, postprocess_times, total_times, latency_times;
    
    // Warm-up runs
    for (int i = 0; i < 10; ++i) {
        DetectorFactory::detect(detector.get(), config, image);
    }
    
    // Measure initial system state
    double initial_memory = getCurrentMemoryUsageMB();
    double initial_sys_memory = SystemMonitor::getSystemMemoryUsage();
    SystemMonitor::getCPUUsage(); // Initialize CPU monitoring
    
    // Enhanced benchmark runs
    std::vector<double> cpu_usage_samples, gpu_usage_samples, gpu_memory_samples;
    
    for (int i = 0; i < iterations; ++i) {
        // Monitor system resources
        double cpu_usage = SystemMonitor::getCPUUsage();
        auto gpu_stats = SystemMonitor::getGPUUsage();
        
        cpu_usage_samples.push_back(cpu_usage);
        gpu_usage_samples.push_back(gpu_stats.first);
        gpu_memory_samples.push_back(gpu_stats.second);
        
        // Time the detection
        cv::TickMeter tm;
        tm.start();
        
        auto total_start = std::chrono::high_resolution_clock::now();
        auto infer_start = std::chrono::high_resolution_clock::now();
        
        auto results = DetectorFactory::detect(detector.get(), config, image);
        
        auto infer_end = std::chrono::high_resolution_clock::now();
        auto total_end = std::chrono::high_resolution_clock::now();
        
        tm.stop();
        
        double infer_time = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        double total_time = std::chrono::duration<double, std::milli>(total_end - total_start).count();
        double latency = tm.getTimeMilli();
        
        preprocess_times.push_back(0.0);
        inference_times.push_back(infer_time);
        postprocess_times.push_back(0.0);
        total_times.push_back(total_time);
        latency_times.push_back(latency);
    }
    
    // Calculate final memory usage
    double final_memory = getCurrentMemoryUsageMB();
    double final_sys_memory = SystemMonitor::getSystemMemoryUsage();
    metrics.memory_mb = final_memory - initial_memory;
    metrics.system_memory_used_mb = final_sys_memory - initial_sys_memory;
    
    // Calculate statistics
    auto calc_avg = [](const std::vector<double>& values) {
        return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    
    auto calc_min_max = [](const std::vector<double>& values) {
        auto minmax = std::minmax_element(values.begin(), values.end());
        return std::make_pair(*minmax.first, *minmax.second);
    };
    
    metrics.preprocess_avg_ms = calc_avg(preprocess_times);
    metrics.inference_avg_ms = calc_avg(inference_times);
    metrics.postprocess_avg_ms = calc_avg(postprocess_times);
    metrics.total_avg_ms = calc_avg(total_times);
    metrics.fps = 1000.0 / metrics.total_avg_ms;
    
    // Enhanced latency statistics
    metrics.latency_avg_ms = calc_avg(latency_times);
    auto latency_minmax = calc_min_max(latency_times);
    metrics.latency_min_ms = latency_minmax.first;
    metrics.latency_max_ms = latency_minmax.second;
    
    // System resource statistics
    metrics.cpu_usage_percent = calc_avg(cpu_usage_samples);
    metrics.gpu_usage_percent = calc_avg(gpu_usage_samples);
    metrics.gpu_memory_used_mb = calc_avg(gpu_memory_samples);
    
    metrics.frame_count = iterations;
    
    return metrics;
}

// Enhanced video benchmark
PerformanceMetrics benchmark_video_comprehensive(const BenchmarkConfig& config,
                                               const std::string& video_path) {
    PerformanceMetrics metrics;
    metrics.environment_type = config.use_gpu ? "GPU" : "CPU";
    
    // Measure model loading time
    auto load_start = std::chrono::high_resolution_clock::now();
    auto detector = DetectorFactory::createDetector(config);
    auto load_end = std::chrono::high_resolution_clock::now();
    metrics.load_time_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();
    
    // Open video
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        throw std::runtime_error("Could not open video: " + video_path);
    }
    
    std::vector<double> frame_times, latency_times;
    std::vector<double> cpu_usage_samples, gpu_usage_samples, gpu_memory_samples;
    
    // Measure initial system state
    double initial_memory = getCurrentMemoryUsageMB();
    double initial_sys_memory = SystemMonitor::getSystemMemoryUsage();
    SystemMonitor::getCPUUsage();
    
    auto start_time = std::chrono::high_resolution_clock::now();
    int frame_count = 0;
    cv::Mat frame;
    
    while (cap.read(frame) && !frame.empty()) {
        // Monitor system resources
        double cpu_usage = SystemMonitor::getCPUUsage();
        auto gpu_stats = SystemMonitor::getGPUUsage();
        
        cpu_usage_samples.push_back(cpu_usage);
        gpu_usage_samples.push_back(gpu_stats.first);
        gpu_memory_samples.push_back(gpu_stats.second);
        
        // Time frame processing
        cv::TickMeter tm;
        tm.start();
        
        auto frame_start = std::chrono::high_resolution_clock::now();
        auto results = DetectorFactory::detect(detector.get(), config, frame);
        auto frame_end = std::chrono::high_resolution_clock::now();
        
        tm.stop();
        
        double frame_time = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
        double latency = tm.getTimeMilli();
        
        frame_times.push_back(frame_time);
        latency_times.push_back(latency);
        
        frame_count++;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    
    double final_memory = getCurrentMemoryUsageMB();
    double final_sys_memory = SystemMonitor::getSystemMemoryUsage();
    metrics.memory_mb = final_memory - initial_memory;
    metrics.system_memory_used_mb = final_sys_memory - initial_sys_memory;
    
    // Calculate statistics
    auto calc_avg = [](const std::vector<double>& values) {
        return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    
    auto calc_min_max = [](const std::vector<double>& values) {
        auto minmax = std::minmax_element(values.begin(), values.end());
        return std::make_pair(*minmax.first, *minmax.second);
    };
    
    metrics.frame_count = frame_count;
    metrics.total_avg_ms = calc_avg(frame_times);
    metrics.fps = (frame_count * 1000.0) / total_time;
    
    // Enhanced latency statistics
    metrics.latency_avg_ms = calc_avg(latency_times);
    auto latency_minmax = calc_min_max(latency_times);
    metrics.latency_min_ms = latency_minmax.first;
    metrics.latency_max_ms = latency_minmax.second;
    
    // System resource statistics
    metrics.cpu_usage_percent = calc_avg(cpu_usage_samples);
    metrics.gpu_usage_percent = calc_avg(gpu_usage_samples);
    metrics.gpu_memory_used_mb = calc_avg(gpu_memory_samples);
    
    return metrics;
}

// Enhanced camera benchmark
PerformanceMetrics benchmark_camera_comprehensive(const BenchmarkConfig& config,
                                                 int camera_id = 0,
                                                 int duration_seconds = 30) {
    PerformanceMetrics metrics;
    metrics.environment_type = config.use_gpu ? "GPU" : "CPU";
    
    // Measure model loading time
    auto load_start = std::chrono::high_resolution_clock::now();
    auto detector = DetectorFactory::createDetector(config);
    auto load_end = std::chrono::high_resolution_clock::now();
    metrics.load_time_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();
    
    // Open camera
    cv::VideoCapture cap(camera_id);
    if (!cap.isOpened()) {
        throw std::runtime_error("Could not open camera with ID: " + std::to_string(camera_id));
    }
    
    std::vector<double> frame_times, latency_times;
    std::vector<double> cpu_usage_samples, gpu_usage_samples, gpu_memory_samples;
    
    // Measure initial system state
    double initial_memory = getCurrentMemoryUsageMB();
    double initial_sys_memory = SystemMonitor::getSystemMemoryUsage();
    SystemMonitor::getCPUUsage();
    
    auto start_time = std::chrono::high_resolution_clock::now();
    auto end_target = start_time + std::chrono::seconds(duration_seconds);
    int frame_count = 0;
    cv::Mat frame;
    
    std::cout << "Running camera benchmark for " << duration_seconds << " seconds..." << std::endl;
    
    while (std::chrono::high_resolution_clock::now() < end_target) {
        if (!cap.read(frame) || frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        
        // Monitor system resources
        double cpu_usage = SystemMonitor::getCPUUsage();
        auto gpu_stats = SystemMonitor::getGPUUsage();
        
        cpu_usage_samples.push_back(cpu_usage);
        gpu_usage_samples.push_back(gpu_stats.first);
        gpu_memory_samples.push_back(gpu_stats.second);
        
        // Time frame processing
        cv::TickMeter tm;
        tm.start();
        
        auto frame_start = std::chrono::high_resolution_clock::now();
        auto results = DetectorFactory::detect(detector.get(), config, frame);
        auto frame_end = std::chrono::high_resolution_clock::now();
        
        tm.stop();
        
        double frame_time = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
        double latency = tm.getTimeMilli();
        
        frame_times.push_back(frame_time);
        latency_times.push_back(latency);
        
        frame_count++;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    
    double final_memory = getCurrentMemoryUsageMB();
    double final_sys_memory = SystemMonitor::getSystemMemoryUsage();
    metrics.memory_mb = final_memory - initial_memory;
    metrics.system_memory_used_mb = final_sys_memory - initial_sys_memory;
    
    // Calculate statistics
    auto calc_avg = [](const std::vector<double>& values) {
        return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    
    auto calc_min_max = [](const std::vector<double>& values) {
        auto minmax = std::minmax_element(values.begin(), values.end());
        return std::make_pair(*minmax.first, *minmax.second);
    };
    
    metrics.frame_count = frame_count;
    metrics.total_avg_ms = calc_avg(frame_times);
    metrics.fps = (frame_count * 1000.0) / total_time;
    
    // Enhanced latency statistics
    metrics.latency_avg_ms = calc_avg(latency_times);
    auto latency_minmax = calc_min_max(latency_times);
    metrics.latency_min_ms = latency_minmax.first;
    metrics.latency_max_ms = latency_minmax.second;
    
    // System resource statistics
    metrics.cpu_usage_percent = calc_avg(cpu_usage_samples);
    metrics.gpu_usage_percent = calc_avg(gpu_usage_samples);
    metrics.gpu_memory_used_mb = calc_avg(gpu_memory_samples);
    
    return metrics;
}

// CSV output functions
void printCSVHeader() {
    std::cout << "model_type,task_type,environment,device,threads,precision,load_ms,preprocess_ms,inference_ms,postprocess_ms,total_ms,fps,memory_mb,system_memory_mb,cpu_usage_%,gpu_usage_%,gpu_memory_mb,latency_avg_ms,latency_min_ms,latency_max_ms,map_score,frame_count\n";
}

void printCSVRow(const BenchmarkConfig& config, const PerformanceMetrics& metrics) {
    std::cout << config.model_type << ","
              << config.task_type << ","
              << metrics.environment_type << ","
              << (config.use_gpu ? "gpu" : "cpu") << ","
              << config.thread_count << ","
              << config.precision << ","
              << std::fixed << std::setprecision(3)
              << metrics.load_time_ms << ","
              << metrics.preprocess_avg_ms << ","
              << metrics.inference_avg_ms << ","
              << metrics.postprocess_avg_ms << ","
              << metrics.total_avg_ms << ","
              << metrics.fps << ","
              << metrics.memory_mb << ","
              << metrics.system_memory_used_mb << ","
              << metrics.cpu_usage_percent << ","
              << metrics.gpu_usage_percent << ","
              << metrics.gpu_memory_used_mb << ","
              << metrics.latency_avg_ms << ","
              << metrics.latency_min_ms << ","
              << metrics.latency_max_ms << ","
              << metrics.map_score << ","
              << metrics.frame_count << std::endl;
}

// Configuration parsing
BenchmarkConfig parseConfig(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "Usage: " << argv[0] << " <mode> <model_type> <task_type> <model_path> <labels_path> <input_path> [options]\n"
                << "Modes: image, video, camera\n"
                << "Model types: yolo5, yolo7, yolo8, yolo9, yolo10, yolo11, yolo12\n"
                << "Task types: detection, segmentation, obb, pose\n"
                << "Options: --gpu, --cpu, --threads=N, --quantized, --iterations=N\n";
        throw std::runtime_error("Invalid arguments");
    }
    
    BenchmarkConfig config;
    config.model_type = argv[2];
    config.task_type = argv[3];
    config.model_path = argv[4];
    config.labels_path = argv[5];
    
    // Parse optional arguments
    for (int i = 7; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--gpu" || arg == "gpu") {
            config.use_gpu = true;
        } else if (arg == "--cpu" || arg == "cpu") {
            config.use_gpu = false;
        } else if (arg.substr(0, 10) == "--threads=") {
            config.thread_count = std::stoi(arg.substr(10));
        } else if (arg == "--quantized") {
            config.quantized = true;
            config.precision = "int8";
        }
    }
    
    return config;
}

// Main function
int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <mode> <model_type> <task_type> <model_path> <labels_path> <input_path> [options]\n"
                      << "Modes: image, video, camera, comprehensive\n"
                      << "Model types: yolo5, yolo7, yolo8, yolo9, yolo10, yolo11, yolo12\n"
                      << "Task types: detection, segmentation, obb, pose\n"
                      << "Options: --gpu, --cpu, --threads=N, --quantized, --iterations=N, --duration=N\n"
                      << "\nExamples:\n"
                      << "  " << argv[0] << " image yolo11 detection models/yolo11n.onnx models/coco.names data/dog.jpg --gpu\n"
                      << "  " << argv[0] << " video yolo8 detection models/yolov8n.onnx models/coco.names data/dogs.mp4 --cpu\n"
                      << "  " << argv[0] << " comprehensive  # Run all supported combinations\n";
            return 1;
        }

        std::string mode = argv[1];
        
        if (mode == "comprehensive") {
            std::cout << "🚀 YOLO Performance Analyzer - Advanced System Monitoring & Benchmarking...\n";
            
            // Create results directory
            std::filesystem::create_directories("results");
            
            // Test configurations focusing on available models (original + quantized)
            std::vector<std::tuple<std::string, std::string, std::string>> test_configs = {
                {"yolo11", "detection", "models/yolo11n.onnx"},
                {"yolo8", "detection", "models/yolov8n.onnx"},
                {"yolo11_quantized", "detection", "quantized_models/yolo11n_quantized.onnx"},
                {"yolo8_quantized", "detection", "quantized_models/yolov8n_quantized.onnx"},
            };
            
            std::vector<bool> gpu_configs = {false, true}; // CPU and GPU
            std::vector<int> iteration_configs = {50, 100};
            
            // Create CSV results file
            std::string results_file = "results/comprehensive_benchmark_" + std::to_string(std::time(nullptr)) + ".csv";
            std::ofstream file(results_file);
            if (file.is_open()) {
                std::streambuf* orig = std::cout.rdbuf();
                std::cout.rdbuf(file.rdbuf());
                printCSVHeader();
                std::cout.rdbuf(orig);
                file.close();
            }
            
            std::cout << "Starting comprehensive benchmark...\n";
            
            for (const auto& [model_type, task_type, model_path] : test_configs) {
                // Check if model file exists
                if (!std::filesystem::exists(model_path)) {
                    std::cerr << "Skipping " << model_type << "/" << task_type << " - model not found: " << model_path << "\n";
                    continue;
                }
                
                for (bool use_gpu : gpu_configs) {
                    for (int iterations : iteration_configs) {
                        BenchmarkConfig config;
                        config.model_type = model_type;
                        config.task_type = task_type;
                        config.model_path = model_path;
                        config.labels_path = "models/coco.names";
                        config.use_gpu = use_gpu;
                        config.thread_count = 1;
                        
                        try {
                            std::cout << "Testing " << model_type << "/" << task_type 
                                      << " on " << (use_gpu ? "GPU" : "CPU") 
                                      << " with " << iterations << " iterations...\n";
                            
                            // Run image benchmark
                            auto image_metrics = benchmark_image_comprehensive(config, "data/dog.jpg", iterations);
                            
                            // Append to results file
                            std::ofstream append_file(results_file, std::ios::app);
                            if (append_file.is_open()) {
                                std::streambuf* orig = std::cout.rdbuf();
                                std::cout.rdbuf(append_file.rdbuf());
                                printCSVRow(config, image_metrics);
                                std::cout.rdbuf(orig);
                                append_file.close();
                            }
                            
                            // Add small delay to prevent system overload
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            
                        } catch (const std::exception& e) {
                            std::cerr << "Error benchmarking " << model_type << "/" << task_type 
                                    << " on " << (use_gpu ? "GPU" : "CPU") << ": " << e.what() << "\n";
                        }
                    }
                }
            }
            
            std::cout << "Comprehensive benchmark completed!\n";
            std::cout << "Results saved to: " << results_file << "\n";
            
            return 0;
        }
        
        // Parse configuration for single benchmark
        BenchmarkConfig config = parseConfig(argc, argv);
        std::string input_path = argv[6];
        
        int iterations = 100;
        int duration = 30;
        
        // Parse additional options
        for (int i = 7; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.substr(0, 13) == "--iterations=") {
                iterations = std::stoi(arg.substr(13));
            } else if (arg.substr(0, 11) == "--duration=") {
                duration = std::stoi(arg.substr(11));
            }
        }
        
        printCSVHeader();
        
        PerformanceMetrics metrics;
        
        if (mode == "image") {
            metrics = benchmark_image_comprehensive(config, input_path, iterations);
        } else if (mode == "video") {
            metrics = benchmark_video_comprehensive(config, input_path);
        } else if (mode == "camera") {
            int camera_id = std::stoi(input_path);
            metrics = benchmark_camera_comprehensive(config, camera_id, duration);
        } else {
            std::cerr << "Error: Invalid mode '" << mode << "'. Use 'image', 'video', 'camera', or 'comprehensive'.\n";
            return 1;
        }
        
        printCSVRow(config, metrics);
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
