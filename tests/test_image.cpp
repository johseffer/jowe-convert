#include "test_util.hpp"

TEST(image_roundtrip_formats) {
  fs::path dir = tempDir() / "img";
  fs::create_directories(dir);
  fs::path src = dir / "src.png";
  makePng(src, 97, 61, true);
  auto p = jc::probe(src);
  REQUIRE(p.result);
  CHECK(p.info.width == 97 && p.info.height == 61);
  CHECK(p.info.channels == 4);

  for (const char* ext : {"jpg", "png", "webp", "tiff", "bmp", "tga"}) {
    fs::path out = dir / (std::string("out.") + ext);
    auto rep = jc::convert(src, out);
    CHECK_OK(rep);
    CHECK(rep.output.width == 97 && rep.output.height == 61);
    fs::path back = dir / (std::string("back_") + ext + ".png");
    auto rep2 = jc::convert(out, back);
    CHECK_OK(rep2);
    CHECK(rep2.output.width == 97 && rep2.output.height == 61);
  }
}

TEST(image_tiff_lossless) {
  fs::path dir = tempDir() / "tiff";
  fs::create_directories(dir);
  makePng(dir / "a.png", 300, 200, true);
  CHECK_OK(jc::convert(dir / "a.png", dir / "a.tif"));
  CHECK_OK(jc::convert(dir / "a.tif", dir / "b.png"));
  CHECK_OK(jc::convert(dir / "a.png", dir / "a.bmp"));
  CHECK_OK(jc::convert(dir / "b.png", dir / "b.bmp"));
  auto a = readAll(dir / "a.bmp"), b = readAll(dir / "b.bmp");
  CHECK(!a.empty() && a == b);
}

TEST(image_resize_and_quality) {
  fs::path dir = tempDir() / "resize";
  fs::create_directories(dir);
  makePng(dir / "big.png", 640, 320, false);
  jc::ConvertOptions o;
  o.image.maxSize = 100;
  auto rep = jc::convert(dir / "big.png", dir / "small.jpg", o);
  CHECK_OK(rep);
  CHECK(rep.output.width == 100 && rep.output.height == 50);
  o.image.maxSize = 4096;
  rep = jc::convert(dir / "big.png", dir / "same.png", o);
  CHECK(rep.output.width == 640);
  jc::ConvertOptions lo, hi;
  lo.image.quality = 10;
  hi.image.quality = 95;
  auto a = jc::convert(dir / "big.png", dir / "lo.jpg", lo);
  auto b = jc::convert(dir / "big.png", dir / "hi.jpg", hi);
  CHECK(a.output.fileSize < b.output.fileSize);
  auto c = jc::convert(dir / "big.png", dir / "lo.webp", lo);
  auto d = jc::convert(dir / "big.png", dir / "hi.webp", hi);
  CHECK(c.output.fileSize < d.output.fileSize);
}

TEST(image_heic_encode) {
  auto f = jc::findFormat("heic");
  REQUIRE(f.has_value());
  if (!f->canWrite) {
    std::fprintf(stderr, "  skipped: %s\n", f->writeReason.c_str());
    return;
  }
  fs::path dir = tempDir() / "heic";
  fs::create_directories(dir);
  makePng(dir / "a.png", 121, 67, true);
  auto rep = jc::convert(dir / "a.png", dir / "a.heic");
  CHECK_OK(rep);
  auto p = jc::probe(dir / "a.heic");
  REQUIRE(p.result);
  CHECK(p.info.codec == "hvc1");
  CHECK(p.info.width == 121 && p.info.height == 67);
  auto bytes = readAll(dir / "a.heic");
  CHECK(bytes.size() > 100 && std::memcmp(bytes.data() + 4, "ftypheic", 8) == 0);
  if (f->canRead) {
    auto back = jc::convert(dir / "a.heic", dir / "back.png");
    CHECK_OK(back);
    CHECK(back.output.width == 121 && back.output.height == 67);
  }
}

TEST(image_errors) {
  fs::path dir = tempDir() / "imgerr";
  fs::create_directories(dir);
  writeText(dir / "broken.jpg", "\xFF\xD8\xFF not really a jpeg");
  auto rep = jc::convert(dir / "broken.jpg", dir / "x.png");
  CHECK(rep.result.status == jc::Status::Unreadable);
  CHECK(!fs::exists(dir / "x.png"));
  makePng(dir / "a.png", 8, 8, false);
  rep = jc::convert(dir / "a.png", dir / "a.wav");
  CHECK(rep.result.status == jc::Status::KindMismatch);
  rep = jc::convert(dir / "a.png", dir / "a.gif");
  CHECK(rep.result.status == jc::Status::NotWritable);
  jc::ConvertOptions o;
  o.access = [&](const fs::path& p, bool write) { return !write || p.filename() != "denied.jpg"; };
  rep = jc::convert(dir / "a.png", dir / "denied.jpg", o);
  CHECK(rep.result.status == jc::Status::AccessDenied);
}
