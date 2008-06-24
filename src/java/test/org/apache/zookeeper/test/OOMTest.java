/**
 * 
 */
package org.apache.zookeeper.test;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;

import junit.framework.TestCase;

import org.apache.zookeeper.Watcher;
import org.apache.zookeeper.ZooKeeper;
import org.apache.zookeeper.KeeperException;
import org.apache.zookeeper.ZooDefs.Ids;
import org.apache.zookeeper.data.Stat;
import org.apache.zookeeper.proto.WatcherEvent;
import org.apache.zookeeper.server.NIOServerCnxn;
import org.apache.zookeeper.server.ZooKeeperServer;

/**
 * @author breed
 * 
 */
public class OOMTest extends TestCase implements Watcher {
    public void testOOM() throws IOException, InterruptedException, KeeperException {
        // This test takes too long to run!
        if (true)
            return;
        File tmpDir = File.createTempFile("test", ".junit");
        tmpDir = new File(tmpDir + ".dir");
        tmpDir.mkdirs();
        // Grab some memory so that it is easier to cause an
        // OOM condition;
        ArrayList<byte[]> hog = new ArrayList<byte[]>();
        while (true) {
            try {
                hog.add(new byte[1024 * 1024 * 2]);
            } catch (OutOfMemoryError e) {
                hog.remove(0);
                break;
            }
        }
        ZooKeeperServer zks = new ZooKeeperServer(tmpDir, tmpDir, 3000);
        NIOServerCnxn.Factory f = new NIOServerCnxn.Factory(33221);
        f.startup(zks);
        Thread.sleep(2000);
        System.err.println("OOM Stage 0");
        utestPrep();
        System.out.println("Free = " + Runtime.getRuntime().freeMemory()
                + " total = " + Runtime.getRuntime().totalMemory() + " max = "
                + Runtime.getRuntime().maxMemory());
        System.err.println("OOM Stage 1");
        for (int i = 0; i < 1000; i++) {
            System.out.println(i);
            utestExists();
        }
        System.out.println("Free = " + Runtime.getRuntime().freeMemory()
                + " total = " + Runtime.getRuntime().totalMemory() + " max = "
                + Runtime.getRuntime().maxMemory());
        System.err.println("OOM Stage 2");
        for (int i = 0; i < 1000; i++) {
            System.out.println(i);
            utestGet();
        }
        System.out.println("Free = " + Runtime.getRuntime().freeMemory()
                + " total = " + Runtime.getRuntime().totalMemory() + " max = "
                + Runtime.getRuntime().maxMemory());
        System.err.println("OOM Stage 3");
        for (int i = 0; i < 1000; i++) {
            System.out.println(i);
            utestChildren();
        }
        System.out.println("Free = " + Runtime.getRuntime().freeMemory()
                + " total = " + Runtime.getRuntime().totalMemory() + " max = "
                + Runtime.getRuntime().maxMemory());
        hog.get(0)[0] = (byte) 1;
        f.shutdown();
    }

    private void utestExists() throws IOException, InterruptedException, KeeperException {
        ZooKeeper zk = new ZooKeeper("127.0.0.1:33221", 30000, this);
        for (int i = 0; i < 10000; i++) {
            zk.exists("/this/path/doesnt_exist!", true);
        }
        zk.close();
    }

    private void utestPrep() throws IOException,
            InterruptedException, KeeperException {
        ZooKeeper zk = new ZooKeeper("127.0.0.1:33221", 30000, this);
        for (int i = 0; i < 10000; i++) {
            zk.create("/" + i, null, Ids.OPEN_ACL_UNSAFE, 0);
        }
        zk.close();
    }

    private void utestGet() throws IOException, InterruptedException, KeeperException {
        ZooKeeper zk = new ZooKeeper("127.0.0.1:33221", 30000, this);
        for (int i = 0; i < 10000; i++) {
            Stat stat = new Stat();
            zk.getData("/" + i, true, stat);
        }
        zk.close();
    }

    private void utestChildren() throws IOException, InterruptedException, KeeperException {
        ZooKeeper zk = new ZooKeeper("127.0.0.1:33221", 30000, this);
        for (int i = 0; i < 10000; i++) {
            zk.getChildren("/" + i, true);
        }
        zk.close();
    }

    /*
     * (non-Javadoc)
     * 
     * @see org.apache.zookeeper.Watcher#process(org.apache.zookeeper.proto.WatcherEvent)
     */
    public void process(WatcherEvent event) {
        System.err.println("Got event " + event.getType() + " "
                + event.getState() + " " + event.getPath());
    }
}
