#include "cratchet/capture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class capture_test : public ::testing::Test {
 protected:
  void TearDown() override { std::filesystem::remove(path_); }

  std::string write(const std::string& contents) {
    std::ofstream(path_) << contents;
    return path_.string();
  }

 private:
  std::filesystem::path path_ =
      std::filesystem::temp_directory_path() /
      ("cratchet_capture_test_" +
       std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".jsonl");
};

TEST_F(capture_test, reads_frames_in_order) {
  cratchet::capture_reader reader(
      write("{\"recv_ns\": 1795053206583964000, \"msg\": \"{\\\"type\\\":\\\"heartbeat\\\"}\"}\n"
            "{\"msg\": \"second\", \"recv_ns\": 2}\n"));

  // (recv_ns, msg) of the next frame, or (-1, "<eof>").
  auto next = [&reader]() -> std::pair<std::int64_t, std::string> {
    const auto frame = reader.next();
    if (!frame) {
      return {-1, "<eof>"};
    }
    return {frame->recv_ns, std::string(frame->msg)};
  };

  EXPECT_EQ(next(),
            (std::pair<std::int64_t, std::string>{1795053206583964000, R"({"type":"heartbeat"})"}));
  EXPECT_EQ(reader.line_number(), 1U);
  EXPECT_EQ(next(), (std::pair<std::int64_t, std::string>{2, "second"}));
  EXPECT_FALSE(reader.next());
}

TEST_F(capture_test, malformed_line_reports_its_number) {
  cratchet::capture_reader reader(write("{\"recv_ns\": 1, \"msg\": \"ok\"}\n{\"recv_ns\": 2}\n"));
  EXPECT_TRUE(reader.next());
  try {
    (void)reader.next();
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(".jsonl:2:"), std::string::npos) << e.what();
  }
}

TEST(capture, missing_file_throws) {
  EXPECT_THROW(cratchet::capture_reader("/nonexistent/capture.jsonl"), std::runtime_error);
}

}  // namespace
