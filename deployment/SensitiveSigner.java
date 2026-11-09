// SPDX-License-Identifier: Apache-2.0
// Keeps decrypted passwords off the operating-system process command line.
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.jar.JarFile;

public final class SensitiveSigner {
    public static void main(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("Signing tool arguments are missing.");
        BufferedReader reader = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        String keyLine = reader.readLine();
        String storeLine = reader.readLine();
        if (keyLine == null || storeLine == null) throw new IllegalArgumentException("Signing passwords are missing.");
        byte[] keyBytes = Base64.getDecoder().decode(keyLine);
        byte[] storeBytes = Base64.getDecoder().decode(storeLine);
        try {
            String mainClass;
            try (JarFile jar = new JarFile(args[0])) {
                mainClass = jar.getManifest().getMainAttributes().getValue("Main-Class");
            }
            if (mainClass == null) throw new IllegalArgumentException("Signing tool has no main class.");
            ArrayList<String> toolArgs = new ArrayList<>(Arrays.asList(args).subList(1, args.length));
            toolArgs.add("-keyPwd");
            toolArgs.add(new String(keyBytes, StandardCharsets.UTF_8));
            toolArgs.add("-keystorePwd");
            toolArgs.add(new String(storeBytes, StandardCharsets.UTF_8));
            try {
                Class.forName(mainClass).getMethod("main", String[].class)
                    .invoke(null, (Object) toolArgs.toArray(new String[0]));
            } catch (InvocationTargetException error) {
                throw new IllegalStateException("Signing tool execution failed.", error.getCause());
            }
        } finally {
            Arrays.fill(keyBytes, (byte) 0);
            Arrays.fill(storeBytes, (byte) 0);
        }
    }
}
