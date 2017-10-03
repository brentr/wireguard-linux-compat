/* Copyright (C) 2015-2017 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved. */

#include "queueing.h"
#include "device.h"
#include "peer.h"
#include "timers.h"
#include "messages.h"
#include "cookie.h"
#include "socket.h"

#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <net/ip_tunnels.h>

static inline void rx_stats(struct wireguard_peer *peer, size_t len)
{
	struct pcpu_sw_netstats *tstats = get_cpu_ptr(peer->device->dev->tstats);
	u64_stats_update_begin(&tstats->syncp);
	tstats->rx_bytes += len;
	++tstats->rx_packets;
	u64_stats_update_end(&tstats->syncp);
	put_cpu_ptr(tstats);
	peer->rx_bytes += len;
}

static inline void update_latest_addr(struct wireguard_peer *peer, struct sk_buff *skb)
{
	struct endpoint endpoint;
	if (!socket_endpoint_from_skb(&endpoint, skb))
		socket_set_peer_endpoint(peer, &endpoint);
}

#define SKB_TYPE_LE32(skb) ((struct message_header *)(skb)->data)->type

static inline size_t validate_header_len(struct sk_buff *skb)
{
	if (unlikely(skb->len < sizeof(struct message_header)))
		return 0;
	if (SKB_TYPE_LE32(skb) == cpu_to_le32(MESSAGE_DATA) && skb->len >= MESSAGE_MINIMUM_LENGTH)
		return sizeof(struct message_data);
	if (SKB_TYPE_LE32(skb) == cpu_to_le32(MESSAGE_HANDSHAKE_INITIATION) && skb->len == sizeof(struct message_handshake_initiation))
		return sizeof(struct message_handshake_initiation);
	if (SKB_TYPE_LE32(skb) == cpu_to_le32(MESSAGE_HANDSHAKE_RESPONSE) && skb->len == sizeof(struct message_handshake_response))
		return sizeof(struct message_handshake_response);
	if (SKB_TYPE_LE32(skb) == cpu_to_le32(MESSAGE_HANDSHAKE_COOKIE) && skb->len == sizeof(struct message_handshake_cookie))
		return sizeof(struct message_handshake_cookie);
	return 0;
}

static inline int skb_prepare_header(struct sk_buff *skb, struct wireguard_device *wg)
{
	struct udphdr *udp;
	size_t data_offset, data_len, header_len;
	if (unlikely(skb_examine_untrusted_ip_hdr(skb) != skb->protocol || skb_transport_header(skb) < skb->head || (skb_transport_header(skb) + sizeof(struct udphdr)) > skb_tail_pointer(skb)))
		return -EINVAL; /* Bogus IP header */
	udp = udp_hdr(skb);
	data_offset = (u8 *)udp - skb->data;
	if (unlikely(data_offset > U16_MAX || data_offset + sizeof(struct udphdr) > skb->len))
		return -EINVAL;  /* Packet has offset at impossible location or isn't big enough to have UDP fields*/
	data_len = ntohs(udp->len);
	if (unlikely(data_len < sizeof(struct udphdr) || data_len > skb->len - data_offset))
		return -EINVAL;  /* UDP packet is reporting too small of a size or lying about its size */
	data_len -= sizeof(struct udphdr);
	data_offset = (u8 *)udp + sizeof(struct udphdr) - skb->data;
	if (unlikely(!pskb_may_pull(skb, data_offset + sizeof(struct message_header)) || pskb_trim(skb, data_len + data_offset) < 0))
		return -EINVAL;
	skb_pull(skb, data_offset);
	if (unlikely(skb->len != data_len))
		return -EINVAL; /* Final len does not agree with calculated len */
	header_len = validate_header_len(skb);
	if (unlikely(!header_len))
		return -EINVAL;
	__skb_push(skb, data_offset);
	if (unlikely(!pskb_may_pull(skb, data_offset + header_len)))
		return -EINVAL;
	__skb_pull(skb, data_offset);
	return 0;
}

static void receive_handshake_packet(struct wireguard_device *wg, struct sk_buff *skb)
{
	static unsigned long last_under_load = 0; /* Yes this is global, so that our load calculation applies to the whole system. */
	struct wireguard_peer *peer = NULL;
	bool under_load;
	enum cookie_mac_state mac_state;
	bool packet_needs_cookie;

	if (SKB_TYPE_LE32(skb) == cpu_to_le32(MESSAGE_HANDSHAKE_COOKIE)) {
		net_dbg_skb_ratelimited("%s: Receiving cookie response from %pISpfsc\n", wg->dev->name, skb);
		cookie_message_consume((struct message_handshake_cookie *)skb->data, wg);
		return;
	}

	under_load = skb_queue_len(&wg->incoming_handshakes) >= MAX_QUEUED_INCOMING_HANDSHAKES / 8;
	if (under_load)
		last_under_load = jiffies;
	else
		under_load = time_is_after_jiffies(last_under_load + HZ);
	mac_state = cookie_validate_packet(&wg->cookie_checker, skb, under_load);
	if ((under_load && mac_state == VALID_MAC_WITH_COOKIE) || (!under_load && mac_state == VALID_MAC_BUT_NO_COOKIE))
		packet_needs_cookie = false;
	else if (under_load && mac_state == VALID_MAC_BUT_NO_COOKIE)
		packet_needs_cookie = true;
	else {
		net_dbg_skb_ratelimited("%s: Invalid MAC of handshake, dropping packet from %pISpfsc\n", wg->dev->name, skb);
		return;
	}

	switch (SKB_TYPE_LE32(skb)) {
	case cpu_to_le32(MESSAGE_HANDSHAKE_INITIATION): {
		struct message_handshake_initiation *message = (struct message_handshake_initiation *)skb->data;
		if (packet_needs_cookie) {
			packet_send_handshake_cookie(wg, skb, message->sender_index);
			return;
		}
		peer = noise_handshake_consume_initiation(message, wg);
		if (unlikely(!peer)) {
			net_dbg_skb_ratelimited("%s: Invalid handshake initiation from %pISpfsc\n", wg->dev->name, skb);
			return;
		}
		update_latest_addr(peer, skb);
		net_dbg_ratelimited("%s: Receiving handshake initiation from peer %Lu (%pISpfsc)\n", wg->dev->name, peer->internal_id, &peer->endpoint.addr);
		packet_send_handshake_response(peer);
		break;
	}
	case cpu_to_le32(MESSAGE_HANDSHAKE_RESPONSE): {
		struct message_handshake_response *message = (struct message_handshake_response *)skb->data;
		if (packet_needs_cookie) {
			packet_send_handshake_cookie(wg, skb, message->sender_index);
			return;
		}
		peer = noise_handshake_consume_response(message, wg);
		if (unlikely(!peer)) {
			net_dbg_skb_ratelimited("%s: Invalid handshake response from %pISpfsc\n", wg->dev->name, skb);
			return;
		}
		update_latest_addr(peer, skb);
		net_dbg_ratelimited("%s: Receiving handshake response from peer %Lu (%pISpfsc)\n", wg->dev->name, peer->internal_id, &peer->endpoint.addr);
		if (noise_handshake_begin_session(&peer->handshake, &peer->keypairs)) {
			timers_session_derived(peer);
			timers_handshake_complete(peer);
			/* Calling this function will either send any existing packets in the queue
			 * and not send a keepalive, which is the best case, Or, if there's nothing
			 * in the queue, it will send a keepalive, in order to give immediate
			 * confirmation of the session. */
			packet_send_keepalive(peer);
		}
		break;
	}
	default:
		WARN(1, "Somehow a wrong type of packet wound up in the handshake queue!\n");
		return;
	}

	BUG_ON(!peer);

	rx_stats(peer, skb->len);
	timers_any_authenticated_packet_received(peer);
	timers_any_authenticated_packet_traversal(peer);
	peer_put(peer);
}

void packet_handshake_receive_worker(struct work_struct *work)
{
	struct wireguard_device *wg = container_of(work, struct multicore_worker, work)->ptr;
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wg->incoming_handshakes)) != NULL) {
		receive_handshake_packet(wg, skb);
		dev_kfree_skb(skb);
		cond_resched();
	}
}

static inline void keep_key_fresh(struct wireguard_peer *peer)
{
	struct noise_keypair *keypair;
	bool send = false;
	if (peer->sent_lastminute_handshake)
		return;

	rcu_read_lock_bh();
	keypair = rcu_dereference_bh(peer->keypairs.current_keypair);
	if (likely(keypair && keypair->sending.is_valid) && keypair->i_am_the_initiator &&
	    unlikely(time_is_before_eq_jiffies64(keypair->sending.birthdate + REJECT_AFTER_TIME - KEEPALIVE_TIMEOUT - REKEY_TIMEOUT)))
		send = true;
	rcu_read_unlock_bh();

	if (send) {
		peer->sent_lastminute_handshake = true;
		packet_send_queued_handshake_initiation(peer, false);
	}
}

static inline bool skb_decrypt(struct sk_buff *skb, struct noise_symmetric_key *key)
{
	struct scatterlist sg[MAX_SKB_FRAGS * 2 + 1];
	struct sk_buff *trailer;
	int num_frags;

	if (unlikely(!key))
		return false;

	if (unlikely(!key->is_valid || time_is_before_eq_jiffies64(key->birthdate + REJECT_AFTER_TIME) || key->counter.receive.counter >= REJECT_AFTER_MESSAGES)) {
		key->is_valid = false;
		return false;
	}

	PACKET_CB(skb)->nonce = le64_to_cpu(((struct message_data *)skb->data)->counter);
	skb_pull(skb, sizeof(struct message_data));
	num_frags = skb_cow_data(skb, 0, &trailer);
	if (unlikely(num_frags < 0 || num_frags > ARRAY_SIZE(sg)))
		return false;

	sg_init_table(sg, num_frags);
	if (skb_to_sgvec(skb, sg, 0, skb->len) <= 0)
		return false;

	if (!chacha20poly1305_decrypt_sg(sg, sg, skb->len, NULL, 0, PACKET_CB(skb)->nonce, key->key))
		return false;

	return !pskb_trim(skb, skb->len - noise_encrypted_len(0));
}

/* This is RFC6479, a replay detection bitmap algorithm that avoids bitshifts */
static inline bool counter_validate(union noise_counter *counter, u64 their_counter)
{
	bool ret = false;
	unsigned long index, index_current, top, i;
	spin_lock_bh(&counter->receive.lock);

	if (unlikely(counter->receive.counter >= REJECT_AFTER_MESSAGES + 1 || their_counter >= REJECT_AFTER_MESSAGES))
		goto out;

	++their_counter;

	if (unlikely((COUNTER_WINDOW_SIZE + their_counter) < counter->receive.counter))
		goto out;

	index = their_counter >> ilog2(BITS_PER_LONG);

	if (likely(their_counter > counter->receive.counter)) {
		index_current = counter->receive.counter >> ilog2(BITS_PER_LONG);
		top = min_t(unsigned long, index - index_current, COUNTER_BITS_TOTAL / BITS_PER_LONG);
		for (i = 1; i <= top; ++i)
			counter->receive.backtrack[(i + index_current) & ((COUNTER_BITS_TOTAL / BITS_PER_LONG) - 1)] = 0;
		counter->receive.counter = their_counter;
	}

	index &= (COUNTER_BITS_TOTAL / BITS_PER_LONG) - 1;
	ret = !test_and_set_bit(their_counter & (BITS_PER_LONG - 1), &counter->receive.backtrack[index]);

out:
	spin_unlock_bh(&counter->receive.lock);
	return ret;
}
#include "selftest/counter.h"

static void packet_consume_data_done(struct sk_buff *skb, struct wireguard_peer *peer, struct endpoint *endpoint, bool used_new_key)
{
	struct net_device *dev = peer->device->dev;
	struct wireguard_peer *routed_peer;
	unsigned int len;

	socket_set_peer_endpoint(peer, endpoint);

	if (unlikely(used_new_key)) {
		timers_handshake_complete(peer);
		packet_send_staged_packets(peer);
	}

	keep_key_fresh(peer);

	/* A packet with length 0 is a keepalive packet */
	if (unlikely(!skb->len)) {
		net_dbg_ratelimited("%s: Receiving keepalive packet from peer %Lu (%pISpfsc)\n", dev->name, peer->internal_id, &peer->endpoint.addr);
		goto packet_processed;
	}

	if (unlikely(skb_network_header(skb) < skb->head))
		goto dishonest_packet_size;
	if (unlikely(!(pskb_network_may_pull(skb, sizeof(struct iphdr)) && (ip_hdr(skb)->version == 4 || (ip_hdr(skb)->version == 6 && pskb_network_may_pull(skb, sizeof(struct ipv6hdr)))))))
		goto dishonest_packet_type;

	skb->dev = dev;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->protocol = skb_examine_untrusted_ip_hdr(skb);
	if (skb->protocol == htons(ETH_P_IP)) {
		len = ntohs(ip_hdr(skb)->tot_len);
		if (unlikely(len < sizeof(struct iphdr)))
			goto dishonest_packet_size;
		if (INET_ECN_is_ce(PACKET_CB(skb)->ds))
			IP_ECN_set_ce(ip_hdr(skb));
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		len = ntohs(ipv6_hdr(skb)->payload_len) + sizeof(struct ipv6hdr);
		if (INET_ECN_is_ce(PACKET_CB(skb)->ds))
			IP6_ECN_set_ce(skb, ipv6_hdr(skb));
	} else
		goto dishonest_packet_type;

	if (unlikely(len > skb->len))
		goto dishonest_packet_size;
	if (unlikely(pskb_trim(skb, len)))
		goto packet_processed;

	timers_data_received(peer);

	routed_peer = routing_table_lookup_src(&peer->device->peer_routing_table, skb);
	peer_put(routed_peer); /* We don't need the extra reference. */

	if (unlikely(routed_peer != peer))
		goto dishonest_packet_peer;

	len = skb->len;
	if (unlikely(netif_receive_skb(skb) == NET_RX_DROP)) {
		++dev->stats.rx_dropped;
		net_dbg_ratelimited("%s: Failed to give packet to userspace from peer %Lu (%pISpfsc)\n", dev->name, peer->internal_id, &peer->endpoint.addr);
	} else
		rx_stats(peer, len);
	goto continue_processing;

dishonest_packet_peer:
	net_dbg_skb_ratelimited("%s: Packet has unallowed src IP (%pISc) from peer %Lu (%pISpfsc)\n", dev->name, skb, peer->internal_id, &peer->endpoint.addr);
	++dev->stats.rx_errors;
	++dev->stats.rx_frame_errors;
	goto packet_processed;
dishonest_packet_type:
	net_dbg_ratelimited("%s: Packet is neither ipv4 nor ipv6 from peer %Lu (%pISpfsc)\n", dev->name, peer->internal_id, &peer->endpoint.addr);
	++dev->stats.rx_errors;
	++dev->stats.rx_frame_errors;
	goto packet_processed;
dishonest_packet_size:
	net_dbg_ratelimited("%s: Packet has incorrect size from peer %Lu (%pISpfsc)\n", dev->name, peer->internal_id, &peer->endpoint.addr);
	++dev->stats.rx_errors;
	++dev->stats.rx_length_errors;
	goto packet_processed;
packet_processed:
	dev_kfree_skb(skb);
continue_processing:
	timers_any_authenticated_packet_received(peer);
	timers_any_authenticated_packet_traversal(peer);
}

void packet_rx_worker(struct work_struct *work)
{
	struct crypt_ctx *ctx;
	struct crypt_queue *queue = container_of(work, struct crypt_queue, work);
	struct sk_buff *skb;

	local_bh_disable();
	while ((ctx = queue_first_per_peer(queue)) != NULL && atomic_read(&ctx->is_finished)) {
		queue_dequeue(queue);
		if (likely((skb = ctx->skb) != NULL)) {
			if (likely(counter_validate(&ctx->keypair->receiving.counter, PACKET_CB(skb)->nonce))) {
				skb_reset(skb);
				packet_consume_data_done(skb, ctx->peer, &ctx->endpoint, noise_received_with_keypair(&ctx->peer->keypairs, ctx->keypair));
			}
			else {
				net_dbg_ratelimited("%s: Packet has invalid nonce %Lu (max %Lu)\n", ctx->peer->device->dev->name, PACKET_CB(ctx->skb)->nonce, ctx->keypair->receiving.counter.receive.counter);
				dev_kfree_skb(skb);
			}
		}
		noise_keypair_put(ctx->keypair);
		peer_put(ctx->peer);
		kmem_cache_free(crypt_ctx_cache, ctx);
	}
	local_bh_enable();
}

void packet_decrypt_worker(struct work_struct *work)
{
	struct crypt_ctx *ctx;
	struct crypt_queue *queue = container_of(work, struct multicore_worker, work)->ptr;
	struct wireguard_peer *peer;

	while ((ctx = queue_dequeue_per_device(queue)) != NULL) {
		if (unlikely(socket_endpoint_from_skb(&ctx->endpoint, ctx->skb) < 0 || !skb_decrypt(ctx->skb, &ctx->keypair->receiving))) {
			dev_kfree_skb(ctx->skb);
			ctx->skb = NULL;
		}
		/* Dereferencing ctx is unsafe once ctx->is_finished == true, so
		 * we take a reference here first. */
		peer = peer_rcu_get(ctx->peer);
		atomic_set(&ctx->is_finished, true);
		queue_work_on(cpumask_choose_online(&peer->serial_work_cpu, peer->internal_id), peer->device->packet_crypt_wq, &peer->rx_queue.work);
		peer_put(peer);
	}
}

static void packet_consume_data(struct wireguard_device *wg, struct sk_buff *skb)
{
	struct crypt_ctx *ctx;
	struct noise_keypair *keypair;
	__le32 idx = ((struct message_data *)skb->data)->key_idx;

	rcu_read_lock_bh();
	keypair = noise_keypair_get((struct noise_keypair *)index_hashtable_lookup(&wg->index_hashtable, INDEX_HASHTABLE_KEYPAIR, idx));
	rcu_read_unlock_bh();
	if (unlikely(!keypair)) {
		dev_kfree_skb(skb);
		return;
	}

	ctx = kmem_cache_alloc(crypt_ctx_cache, GFP_ATOMIC);
	if (unlikely(!ctx)) {
		dev_kfree_skb(skb);
		peer_put(ctx->keypair->entry.peer);
		noise_keypair_put(keypair);
		return;
	}
	atomic_set(&ctx->is_finished, false);
	ctx->keypair = keypair;
	ctx->skb = skb;
	/* We already have a reference to peer from index_hashtable_lookup. */
	ctx->peer = ctx->keypair->entry.peer;

	if (likely(queue_enqueue_per_device_and_peer(&wg->decrypt_queue, &ctx->peer->rx_queue, ctx, wg->packet_crypt_wq, &wg->decrypt_queue.last_cpu)))
		return; /* Successful. No need to drop references below. */

	noise_keypair_put(ctx->keypair);
	peer_put(ctx->peer);
	dev_kfree_skb(ctx->skb);
	kmem_cache_free(crypt_ctx_cache, ctx);
}

void packet_receive(struct wireguard_device *wg, struct sk_buff *skb)
{
	if (unlikely(skb_prepare_header(skb, wg) < 0))
		goto err;
	switch (SKB_TYPE_LE32(skb)) {
	case cpu_to_le32(MESSAGE_HANDSHAKE_INITIATION):
	case cpu_to_le32(MESSAGE_HANDSHAKE_RESPONSE):
	case cpu_to_le32(MESSAGE_HANDSHAKE_COOKIE): {
		int cpu;
		if (skb_queue_len(&wg->incoming_handshakes) > MAX_QUEUED_INCOMING_HANDSHAKES) {
			net_dbg_skb_ratelimited("%s: Too many handshakes queued, dropping packet from %pISpfsc\n", wg->dev->name, skb);
			goto err;
		}
		skb_queue_tail(&wg->incoming_handshakes, skb);
		/* Queues up a call to packet_process_queued_handshake_packets(skb): */
		cpu = cpumask_next_online(&wg->incoming_handshake_cpu);
		queue_work_on(cpu, wg->handshake_receive_wq, &per_cpu_ptr(wg->incoming_handshakes_worker, cpu)->work);
		break;
	}
	case cpu_to_le32(MESSAGE_DATA):
		PACKET_CB(skb)->ds = ip_tunnel_get_dsfield(ip_hdr(skb), skb);
		packet_consume_data(wg, skb);
		break;
	default:
		net_dbg_skb_ratelimited("%s: Invalid packet from %pISpfsc\n", wg->dev->name, skb);
		goto err;
	}
	return;

err:
	dev_kfree_skb(skb);
}
