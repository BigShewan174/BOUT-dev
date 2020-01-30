#include "gtest/gtest.h"

#include "interpolation.hxx"
#include "interpolation_factory.hxx"
#include "unused.hxx"
#include "bout/mesh.hxx"
#include "options.hxx"

#include "test_extras.hxx"

namespace bout {
namespace testing {
// Some variable so we can check if a function was called
bool sentinel_set{false};
} // namespace testing
} // namespace bout

class InterpolationFactoryTest : public FakeMeshFixture {
public:
  InterpolationFactoryTest() : FakeMeshFixture() {
    output_info.disable();
  }
  ~InterpolationFactoryTest() {
    output_info.enable();
    InterpolationFactory::getInstance()->cleanup();
    bout::testing::sentinel_set = false;
  }
};

class TestInterpolation : public Interpolation {
public:
  TestInterpolation(Mesh* mesh = nullptr) : Interpolation(0, mesh) {}

  /// Callback function for InterpolationFactory
  static Interpolation* CreateTestInterpolation(Mesh* mesh) { return new Bilinear(mesh); }

  void calcWeights(MAYBE_UNUSED(const Field3D& delta_x),
                   MAYBE_UNUSED(const Field3D& delta_z)) override {
    bout::testing::sentinel_set = true;
  };
  void calcWeights(MAYBE_UNUSED(const Field3D& delta_x),
                   MAYBE_UNUSED(const Field3D& delta_z),
                   MAYBE_UNUSED(const BoutMask& mask)) override {
    bout::testing::sentinel_set = true;
  };

  Field3D interpolate(MAYBE_UNUSED(const Field3D& f)) const override {
    return Field3D{-1, localmesh};
  }
  Field3D interpolate(MAYBE_UNUSED(const Field3D& f),
                      MAYBE_UNUSED(const Field3D& delta_x),
                      MAYBE_UNUSED(const Field3D& delta_z)) override {
    return Field3D{-1, localmesh};
  };
  Field3D interpolate(MAYBE_UNUSED(const Field3D& f),
                      MAYBE_UNUSED(const Field3D& delta_x),
                      MAYBE_UNUSED(const Field3D& delta_z),
                      MAYBE_UNUSED(const BoutMask& mask)) override {
    return Field3D{-1, localmesh};
  };
};

TEST_F(InterpolationFactoryTest, GetInstance) {
  EXPECT_NE(InterpolationFactory::getInstance(), nullptr);
}

TEST_F(InterpolationFactoryTest, GetDefaultInterpType) {
  EXPECT_NE(InterpolationFactory::getInstance()->getDefaultInterpType(), "");
}

TEST_F(InterpolationFactoryTest, AddInterpolation) {
  EXPECT_NO_THROW(InterpolationFactory::getInstance()->add(
      [](Mesh* mesh) -> Interpolation* { return new TestInterpolation(mesh); },
      "test_interpolation"));
}

TEST_F(InterpolationFactoryTest, CreateInterpolation) {
  InterpolationFactory::getInstance()->add(
      [](Mesh* mesh) -> Interpolation* { return new TestInterpolation(mesh); },
      "test_interpolation");

  Interpolation* interpolation{nullptr};
  EXPECT_NO_THROW(interpolation = InterpolationFactory::getInstance()->create("test_interpolation"));

  EXPECT_TRUE(IsFieldEqual(interpolation->interpolate(Field3D{}), -1));
}

TEST_F(InterpolationFactoryTest, CreateInterpolationFromOptions) {
  InterpolationFactory::getInstance()->add(
      [](Mesh* mesh) -> Interpolation* { return new TestInterpolation(mesh); },
      "test_interpolation");

  Options::root()["interpolation"]["type"] = "test_interpolation";

  Interpolation* interpolation{nullptr};
  EXPECT_NO_THROW(interpolation = InterpolationFactory::getInstance()->create());

  EXPECT_TRUE(IsFieldEqual(interpolation->interpolate(Field3D{}), -1));
}

TEST_F(InterpolationFactoryTest, CreateUnknownInterpolation) {
  EXPECT_THROW(InterpolationFactory::getInstance()->create("nonsense"), BoutException);
}

TEST_F(InterpolationFactoryTest, Cleanup) {
  InterpolationFactory::getInstance()->add(
      [](Mesh* mesh) -> Interpolation* { return new TestInterpolation(mesh); },
      "to be removed");

  InterpolationFactory::getInstance()->cleanup();

  EXPECT_THROW(InterpolationFactory::getInstance()->create("to be removed"), BoutException);
}
