/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.zookeeper.test;

import java.io.IOException;
import java.util.Date;
import java.util.LinkedList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import org.apache.zookeeper.Watcher;
import org.apache.zookeeper.ZooKeeper;
import org.apache.zookeeper.AsyncCallback.ChildrenCallback;
import org.apache.zookeeper.AsyncCallback.StringCallback;
import org.apache.zookeeper.AsyncCallback.VoidCallback;
import org.apache.zookeeper.ZooDefs.Ids;
import org.apache.zookeeper.proto.WatcherEvent;
import org.junit.Test;


public class SyncCallTest extends ClientBase
    implements Watcher, ChildrenCallback, StringCallback, VoidCallback
{
    private CountDownLatch clientConnected;
    private CountDownLatch opsCount;
    
    List<Integer> results = new LinkedList<Integer>();
    Integer limit = 100 + 1 + 100 + 100;
    
    @Test
    public void testSync() throws Exception {
        try {
            LOG.info("Starting ZK:" + (new Date()).toString());
            opsCount = new CountDownLatch(limit);
            ZooKeeper zk = createClient();
            
            LOG.info("Beginning test:" + (new Date()).toString());
            for(int i = 0; i < 100; i++)
                zk.create("/test" + i, new byte[0], Ids.OPEN_ACL_UNSAFE,
                        0, this, results);
            zk.sync("/test", this, results);
            for(int i = 0; i < 100; i++)
                zk.delete("/test" + i, 0, this, results);
            for(int i = 0; i < 100; i++)
                zk.getChildren("/", this, this, results);

            LOG.info("Submitted all operations:" + (new Date()).toString());
            
            if(!opsCount.await(10000, TimeUnit.MILLISECONDS))
                fail("Haven't received all confirmations" + opsCount.getCount());

            for(int i = 0; i < limit ; i++){
                assertEquals(0, (int) results.get(i));
            }
            
        } catch (IOException e) {
            System.out.println(e.toString());
        } 
    }
    
    private ZooKeeper createClient() throws IOException,InterruptedException{
        clientConnected=new CountDownLatch(1);
        ZooKeeper zk = new ZooKeeper(hostPort, 30000, this);
        if(!clientConnected.await(CONNECTION_TIMEOUT, TimeUnit.MILLISECONDS)){
            fail("Unable to connect to server");
        }
        return zk;
    }
    
    public void process(WatcherEvent event) {
        //LOG.info("Process: " + event.getType() + " " + event.getPath());       
        if (event.getState() == Event.KeeperStateSyncConnected) {
            clientConnected.countDown();
        }
    }

    @SuppressWarnings("unchecked")
    public void processResult(int rc, String path, Object ctx,
            List<String> children) { 
        ((List<Integer>)ctx).add(rc);
        opsCount.countDown();
    }

    @SuppressWarnings("unchecked")
    public void processResult(int rc, String path, Object ctx, String name){
        ((List<Integer>) ctx).add(rc);
        opsCount.countDown();
    
    }

    @SuppressWarnings("unchecked")
    public void processResult(int rc, String path, Object ctx){
        ((List<Integer>) ctx).add(rc);    
        opsCount.countDown();
    
    }
}
