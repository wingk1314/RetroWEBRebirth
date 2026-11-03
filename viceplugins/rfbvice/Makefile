CXX ?= g++
CFLAGS_COMMON := -std=c++17 -fPIC -fvisibility=hidden -Wall -Werror -Wno-error=deprecated-declarations -Wsign-compare `pkg-config --cflags libvncserver`
CFLAGS_debug := $(CFLAGS_COMMON) -g -O0
CFLAGS_release := $(CFLAGS_COMMON) -O3 -DNDEBUG
LDFLAGS_COMMON := -shared -fPIC -pthread `pkg-config --libs libvncserver`
LDFLAGS_debug := $(LDFLAGS_COMMON)
LDFLAGS_release := $(LDFLAGS_COMMON)
SRCS := $(shell find src -name '*.cpp')

define OUTDEFS
OBJS_$(1) := $(SRCS:%.cpp=$(1)/obj/%.o)
DEPS_$(1) := $(SRCS:%.cpp=$(1)/obj/%.d)
endef
$(foreach b,debug release,$(eval $(call OUTDEFS,$(b))))

.PHONY: debug release clean default test

default: release

define BUILDRULES

$(1): $(1)/lib/rfbvice.so

$(1)/lib/rfbvice.so: $(OBJS_$(1))
	@mkdir -p $(1)/lib
	$(CXX) $(CFLAGS_$(1)) $(OBJS_$(1)) -o $(1)/lib/rfbvice.so $(LDFLAGS_$(1))

endef
$(eval $(call BUILDRULES,debug))
$(eval $(call BUILDRULES,release))

define OBJRULE
$(2:%.cpp=$(1)/obj/%.o): $(2)
	@mkdir -p `dirname $(2:%.cpp=$(1)/obj/%.o)`
	$(CXX) $(CFLAGS_$(1)) -Isrc -MMD -c $(2) -o $(2:%.cpp=$(1)/obj/%.o)
endef
$(foreach s,$(SRCS),$(eval $(call OBJRULE,debug,$(s))))
$(foreach s,$(SRCS),$(eval $(call OBJRULE,release,$(s))))

# Tests: unit tests for the frame diff code, and an end-to-end test that loads
# the plugin with a fake host program and checks the result with a real VNC
# client (libvncclient).
test/frame_diff_test: test/frame_diff_test.cpp src/frame_diff.cpp src/frame_diff.hpp
	$(CXX) -std=c++17 -O2 -Wall -Werror -Isrc test/frame_diff_test.cpp src/frame_diff.cpp -o $@

test/e2e_test: test/e2e_test.cpp release/lib/rfbvice.so
	$(CXX) -std=c++17 -O2 -Wall -Werror test/e2e_test.cpp -o $@ -ldl -pthread `pkg-config --cflags --libs libvncclient`

test: test/frame_diff_test test/e2e_test
	./test/frame_diff_test
	./test/e2e_test release/lib/rfbvice.so

clean:
	rm -rf debug release test/frame_diff_test test/e2e_test

-include $(DEPS_debug) $(DEPS_release)
