CXX ?= $(shell command -v g++ 2>/dev/null || command -v clang++ 2>/dev/null || echo g++)
# NOTE: do not add -fvisibility=hidden. Hyprland exposes its globals
# (g_pInputManager, g_pHyprRenderer, ...) as inline variables; hiding the
# plugin's weak definitions makes it bind to private null copies instead of
# the compositor's instances.
CXXFLAGS += -std=c++23 -O3 -fPIC -Wall -Wextra -fno-gnu-unique -MMD -MP $(shell pkg-config --cflags hyprland)
LDFLAGS  += -shared
LDLIBS   += -lGLESv2 -lEGL

TARGET = hyprtilt.so
SRCS   = src/TiltTransformer.cpp src/main.cpp
OBJS   = $(SRCS:.cpp=.o)
DEPS   = $(OBJS:.o=.d)

PREFIX ?= $(HOME)/.local/share/hyprconf/plugins

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

install: $(TARGET)
	mkdir -p $(PREFIX)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	ln -sf $(PREFIX)/$(TARGET) $(PREFIX)/hyprtilt-v3.so

uninstall:
	rm -f $(PREFIX)/$(TARGET) $(PREFIX)/hyprtilt-v3.so

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)

.PHONY: all install uninstall clean
