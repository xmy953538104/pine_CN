// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader;

import java.util.Arrays;

class SelectedContentBridge {
    static native boolean isAvailable();
    static native String inspect(int borrowedFd, String privatePath, String profile);
}

public final class SelectedJniFixture {
    static native String parse(Object[] fields, int[] indices);
    static native void pending();
    private static int passed;

    private static String[] valid() {
        return new String[]{"1", "application", "modern-40", "0100000000001000",
            "cnmt-sha256:" + "AB".repeat(32), "CD".repeat(16), "0100000000001000", "", "", ""};
    }
    private static void require(boolean value, String name) {
        if (!value) throw new AssertionError(name);
        ++passed;
    }
    private static void reject(Object[] fields, int[] indices, String name) {
        try { parse(fields, indices); throw new AssertionError("accepted " + name); }
        catch (IllegalArgumentException expected) {
            require(expected.getMessage().startsWith("selected_") || expected.getMessage().equals("invalid_selected_launch"), "bounded error " + name);
            require(!expected.getMessage().contains("PRIVATE"), "no input echo " + name);
        }
    }
    public static void main(String[] args) {
        System.load(args[0]);
        require(!SelectedContentBridge.isAvailable(), "ordinary native build remains unavailable");
        try {
            SelectedContentBridge.inspect(-1, "/PRIVATE/not-opened/", "modern-40");
            throw new AssertionError("default-OFF inspect succeeded");
        } catch (IllegalStateException expected) {
            require(expected.getMessage().equals("selected_content_unavailable"), "OFF inspection fails closed");
        }
        require(parse(valid(), new int[]{0, -1}).equals(
            "application:0100000000001000:" + "cd".repeat(16) + ":0:0"), "actual Java string/int transfer and normalization");
        String[] aoc = Arrays.copyOf(valid(), 11); aoc[10] = "cnmt-sha256:" + "ef".repeat(32);
        require(parse(aoc, new int[]{255, -1}).endsWith(":255:1"), "explicit AOC and upper program index");
        String[] standalone = valid(); standalone[1] = "standalone_patch"; standalone[2] = "legacy-48";
        require(parse(standalone, new int[]{1, -1}).startsWith("standalone_patch:"), "explicit standalone legacy profile");
        reject(null, new int[]{0, -1}, "null fields");
        reject(valid(), null, "null indices");
        reject(new String[43], new int[]{0, -1}, "array budget before elements");
        reject(new String[9], new int[]{0, -1}, "short fields");
        reject(valid(), new int[]{0}, "short indices");
        reject(valid(), new int[]{0, -1, 0}, "long indices");
        reject(valid(), new int[]{-1, -1}, "negative primary");
        reject(valid(), new int[]{256, -1}, "primary overflow");
        reject(valid(), new int[]{0, 0}, "absent update sentinel");
        for (String invalid : new String[]{null, "PRIVATE".repeat(1000), "\u0000", "\ud800", "\u4e2d", " 1", "2"}) {
            String[] fields = valid(); fields[0] = invalid;
            reject(fields, new int[]{0, -1}, "invalid version transfer");
        }
        Object[] wrongType = Arrays.copyOf(valid(), 10, Object[].class); wrongType[0] = Integer.valueOf(1);
        reject(wrongType, new int[]{0, -1}, "non-string object");
        String[] partial = valid(); partial[7] = "cnmt-sha256:" + "34".repeat(32);
        reject(partial, new int[]{0, 0}, "partial update");
        String[] duplicate = Arrays.copyOf(valid(), 11); duplicate[10] = duplicate[4].toLowerCase();
        reject(duplicate, new int[]{0, -1}, "normalized duplicate group");
        try { pending(); throw new AssertionError("pending exception cleared"); }
        catch (IllegalStateException expected) {
            require(expected.getMessage().equals("synthetic_pending_exception"), "pending VM exception preserved");
        }
        // -Xcheck:jni observes real local references across repeated calls.
        for (int i = 0; i < 4096; ++i) parse(valid(), new int[]{0, -1});
        require(true, "repeated JNI transfer completes");
        System.out.println("PASS " + passed + " actual host-JVM JNI transfer assertions; Android/loader/guest not run");
    }
}
