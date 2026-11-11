-------------------------------- MODULE Vsr --------------------------------
(***************************************************************************)
(* The libvsr replication protocol: Viewstamped Replication Revisited with *)
(* durable state, crashes that lose unsynced log entries, restart rules,   *)
(* view changes without logs in messages, and an entry-by-entry log copy.  *)
(*                                                                         *)
(* The source of truth is src/replica.cpp. Each action names the C++       *)
(* function that it models.                                                *)
(*                                                                         *)
(* Network model. Messages are never removed, so the model covers loss,    *)
(* delay, reordering, and duplication. The model keeps a history of what   *)
(* each sender can have sent, not one record for each message:            *)
(*   plogs[v]   every log that the primary of v held while Normal in v.    *)
(*              A Prepare, Commit, or StartView names a prefix of one.     *)
(*   pcommit[v] the highest commit number that the primary of v sent.      *)
(*   vso[v]     view_start_op_ of view v (the head of the primary log      *)
(*              when it opened v).                                         *)
(*   svc        the set of <<view, sender>> of StartViewChange messages.   *)
(*   dvcs       the set of DoViewChange messages.                          *)
(* The log field in dvcs and acks denotes a collision-free identity.       *)
(* It does not transfer entries. FetchAppend copies one source entry.      *)
(*   acks       all PrepareOk messages.                                  *)
(* A log entry is a record [v |-> view of creation, x |-> client value].   *)
(* A log prefix stands for the checksum chain: two prefixes are equal if   *)
(* and only if their head checksums are equal.                             *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
  Replica,        \* A set of replica identities, for example {0, 1, 2}.
  Value,          \* The client values.
  MaxView,        \* The highest view.
  MaxLog,         \* The longest log.
  MaxCrashes,     \* The total number of crashes.
  \* Mutations. Each one is a known defect. Each must give a counterexample.
  MutJoinNoLength,     \* (a) JoinFinish without `Len >= viewStartOp`.
  MutAckUnsynced,      \* (b) PrepareOk for unsynced entries.
  MutPrimaryContinues  \* (c) A restarted primary stays the Normal primary.

ASSUME Replica \subseteq Nat /\ Replica = 0 .. (Cardinality(Replica) - 1)
ASSUME MaxView \in Nat \ {0} /\ MaxLog \in Nat /\ MaxCrashes \in Nat

N == Cardinality(Replica)
Quorum == N \div 2 + 1
Views == 0 .. MaxView
PrimaryOf(v) == v % N

None == [none |-> TRUE]

VARIABLES
  \* Durable state. The durable log is SubSeq(log[r], 1, synced[r]).
  view, logView,
  \* The written log: the durable part and a volatile tail.
  log, synced,
  \* Volatile state.
  status, commit, dvcSent, fetch,
  \* The network history.
  plogs, pcommit, vso, svc, dvcs, acks,
  \* Ghost state for the properties.
  committed,   \* Every <<op, entry>> that a replica took as committed.
  pcommitted,  \* Every <<op, entry, view>> that a primary committed by quorum.
  crashes

vars == <<view, logView, log, synced, status, commit, dvcSent, fetch,
          plogs, pcommit, vso, svc, dvcs, acks, committed, pcommitted, crashes>>

-----------------------------------------------------------------------------
\* Helpers.

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b
Prefix(s, n) == SubSeq(s, 1, n)
IsPrefix(s, t) == Len(s) <= Len(t) /\ s = Prefix(t, Len(s))
LCP(s, t) ==
  CHOOSE k \in 0 .. Min(Len(s), Len(t)) :
    /\ Prefix(s, k) = Prefix(t, k)
    /\ \A j \in 0 .. Min(Len(s), Len(t)) :
         Prefix(s, j) = Prefix(t, j) => j <= k
DurableLog(r) == Prefix(log[r], synced[r])
IsOpen(v) == plogs[v] # {}

\* The replica's log is a prefix of a log that the primary of its view sent.
\* This is `log_verified_`: C++ sets it at the first message that shows it.
Verified(r) == \E L \in plogs[view[r]] : IsPrefix(log[r], L)

\* <<op, entry>> pairs that replica r takes as committed: ExecuteCommitted.
CommittedAt(r) == { <<n, log[r][n]>> : n \in 1 .. Min(commit[r], Len(log[r])) }
CommittedNow == UNION { CommittedAt(r) : r \in Replica }

\* The DoViewChange order: log_view_ first, then the head op.
Better(a, b) == a.lv > b.lv \/ (a.lv = b.lv /\ Len(a.log) > Len(b.log))

Entry == [v : Views, x : Value]
Status == {"Normal", "ViewChange", "Joining"}

-----------------------------------------------------------------------------
Init ==
  \* Format writes view 0 and log_view 0. On the first Recover, the
  \* backups of view 0 are Normal, and replica 0 (the primary of view 0)
  \* starts a view change to view 1. View 0 is open with an empty log.
  /\ view = [r \in Replica |-> IF r = PrimaryOf(0) THEN 1 ELSE 0]
  /\ logView = [r \in Replica |-> 0]
  /\ log = [r \in Replica |-> <<>>]
  /\ synced = [r \in Replica |-> 0]
  /\ status = [r \in Replica |-> IF r = PrimaryOf(0) THEN "ViewChange" ELSE "Normal"]
  /\ commit = [r \in Replica |-> 0]
  /\ dvcSent = [r \in Replica |-> FALSE]
  /\ fetch = [r \in Replica |-> None]
  /\ plogs = [v \in Views |-> IF v = 0 THEN {<<>>} ELSE {}]
  /\ pcommit = [v \in Views |-> 0]
  /\ vso = [v \in Views |-> 0]
  /\ svc = {<<1, PrimaryOf(0)>>}
  /\ dvcs = {}
  /\ acks = {}
  /\ committed = {}
  /\ pcommitted = {}
  /\ crashes = 0

-----------------------------------------------------------------------------
\* Primary in Normal status.

\* HandleRequest: append a new entry. The write is not synced yet.
ClientRequest(p, x) ==
  /\ status[p] = "Normal" /\ PrimaryOf(view[p]) = p
  /\ Len(log[p]) < MaxLog
  /\ LET nl == Append(log[p], [v |-> view[p], x |-> x])
     IN /\ log' = [log EXCEPT ![p] = nl]
        /\ plogs' = [plogs EXCEPT ![view[p]] = @ \cup {nl}]
  /\ UNCHANGED <<view, logView, synced, status, commit, dvcSent, fetch,
                 pcommit, vso, svc, dvcs, acks, pcommitted, crashes>>

\* AdvanceCommit: commit op n when a quorum holds a synced log that matches
\* up to n in this view. The primary counts itself for synced entries only.
PrimaryCommit(p) ==
  LET v == view[p]
      n == commit[p] + 1
      selfOk == IF synced[p] >= n THEN {p} ELSE {}
      others == { r \in Replica \ {p} :
                    \E a \in acks : /\ a.v = v /\ a.src = r
                                    /\ Len(a.log) >= n
                                    /\ Prefix(a.log, n) = Prefix(log[p], n) }
  IN /\ status[p] = "Normal" /\ PrimaryOf(v) = p
     /\ n <= Len(log[p])
     /\ Cardinality(selfOk \cup others) >= Quorum
     /\ commit' = [commit EXCEPT ![p] = n]
     /\ pcommit' = [pcommit EXCEPT ![v] = Max(@, n)]
     /\ pcommitted' = pcommitted \cup {<<n, log[p][n], v>>}
     /\ UNCHANGED <<view, logView, log, synced, status, dvcSent, fetch,
                    plogs, vso, svc, dvcs, acks, crashes>>

-----------------------------------------------------------------------------
\* Backups.

\* SendPrepareOk: a Normal backup acknowledges its synced log.
\* Mutation (b) acknowledges the written log.
Ack(r) ==
  LET n == IF MutAckUnsynced THEN Len(log[r]) ELSE synced[r]
  IN /\ status[r] = "Normal" /\ PrimaryOf(view[r]) # r
     /\ n > 0
     /\ LET new == [v |-> view[r], src |-> r, log |-> Prefix(log[r], n)]
        IN /\ new \notin acks
           /\ acks' = acks \cup {new}
     /\ UNCHANGED <<view, logView, log, synced, status, commit, dvcSent, fetch,
                    plogs, pcommit, vso, svc, dvcs, pcommitted, crashes>>

\* FollowPrimary and EnterView: a message from the primary of an open view v.
\* A higher view makes the replica sync its log, persist the view, and join.
LearnView(r, v) ==
  /\ IsOpen(v) /\ PrimaryOf(v) # r
  /\ \/ v > view[r]
     \/ v = view[r] /\ status[r] = "ViewChange"
  /\ synced' = [synced EXCEPT ![r] = Len(log[r])]
  /\ view' = [view EXCEPT ![r] = v]
  /\ status' = [status EXCEPT ![r] = "Joining"]
  /\ dvcSent' = [dvcSent EXCEPT ![r] = FALSE]
  /\ fetch' = [fetch EXCEPT ![r] = None]
  /\ UNCHANGED <<logView, log, commit, plogs, pcommit, vso, svc, dvcs, acks,
                 pcommitted, crashes>>

\* Follow: remove the suffix after the longest common prefix.
\* A delayed primary head can name a shorter prefix. TruncateTo protects
\* the operations that this replica already knows are committed.
JoinTruncate(r, k) ==
  /\ status[r] = "Joining"
  /\ k < Len(log[r])
  /\ k >= Min(commit[r], Len(log[r]))
  /\ \E L \in plogs[view[r]] : k = LCP(log[r], L)
  /\ log' = [log EXCEPT ![r] = Prefix(log[r], k)]
  /\ synced' = [synced EXCEPT ![r] = k]
  /\ UNCHANGED <<view, logView, status, commit, dvcSent, fetch, plogs, pcommit,
                 vso, svc, dvcs, acks, pcommitted, crashes>>

\* HandleEntryNormal and HandleEntryUnverified: append the next entry of the
\* primary log when the parent checksum matches. The write is not synced.
JoinAppend(r) ==
  /\ \/ status[r] = "Joining"
     \/ status[r] = "Normal" /\ PrimaryOf(view[r]) # r
  /\ \E L \in plogs[view[r]] :
       /\ Len(L) > Len(log[r])
       /\ IsPrefix(log[r], L)
       /\ log' = [log EXCEPT ![r] = Append(log[r], L[Len(log[r]) + 1])]
  /\ UNCHANGED <<view, logView, synced, status, commit, dvcSent, fetch, plogs,
                 pcommit, vso, svc, dvcs, acks, pcommitted, crashes>>

\* CheckFollowDone and BecomeNormal: sync the log, then persist
\* log_view_ = view_. Mutation (a) leaves out the length guard.
JoinFinish(r) ==
  /\ status[r] = "Joining"
  /\ Verified(r)
  /\ MutJoinNoLength \/ Len(log[r]) >= vso[view[r]]
  /\ synced' = [synced EXCEPT ![r] = Len(log[r])]
  /\ logView' = [logView EXCEPT ![r] = view[r]]
  /\ status' = [status EXCEPT ![r] = "Normal"]
  /\ UNCHANGED <<view, log, commit, dvcSent, fetch, plogs, pcommit, vso, svc,
                 dvcs, acks, pcommitted, crashes>>

\* LearnCommit: only a Normal backup, or a Joining backup with a verified
\* log, takes the commit number of the primary. Any delayed value can come.
LearnCommit(r, c) ==
  /\ \/ status[r] = "Normal" /\ PrimaryOf(view[r]) # r
     \/ status[r] = "Joining" /\ Verified(r)
  /\ c > commit[r] /\ c <= pcommit[view[r]]
  /\ commit' = [commit EXCEPT ![r] = c]
  /\ UNCHANGED <<view, logView, log, synced, status, dvcSent, fetch, plogs,
                 pcommit, vso, svc, dvcs, acks, pcommitted, crashes>>

-----------------------------------------------------------------------------
\* Any replica.

\* Flush and SyncJournal.
Sync(r) ==
  /\ synced[r] < Len(log[r])
  /\ synced' = [synced EXCEPT ![r] = Len(log[r])]
  /\ UNCHANGED <<view, logView, log, status, commit, dvcSent, fetch, plogs,
                 pcommit, vso, svc, dvcs, acks, pcommitted, crashes>>

\* StartViewChange: on a timeout (view + 1) or on a StartViewChange or
\* DoViewChange for a higher view. A Normal primary has no timeout. The
\* journal is synced first, then the view is persisted.
StartViewChange(r, v) ==
  /\ v <= MaxView /\ v > view[r]
  /\ IF status[r] = "Normal" /\ PrimaryOf(view[r]) = r
       THEN \E s \in Replica \ {r} : <<v, s>> \in svc
       ELSE v = view[r] + 1 \/ \E s \in Replica \ {r} : <<v, s>> \in svc
  /\ synced' = [synced EXCEPT ![r] = Len(log[r])]
  /\ view' = [view EXCEPT ![r] = v]
  /\ status' = [status EXCEPT ![r] = "ViewChange"]
  /\ dvcSent' = [dvcSent EXCEPT ![r] = FALSE]
  /\ fetch' = [fetch EXCEPT ![r] = None]
  /\ svc' = svc \cup {<<v, r>>}
  /\ UNCHANGED <<logView, log, commit, plogs, pcommit, vso, dvcs, acks,
                 pcommitted, crashes>>

\* Crash and restart (Recover). Any suffix of the unsynced tail is lost.
\* The volatile state is lost: commit_max_ starts at the checkpoint (0 here).
Crash(r) ==
  /\ crashes < MaxCrashes
  /\ \E k \in synced[r] .. Len(log[r]) :
       LET current == logView[r] = view[r]
           wasPrimary == current /\ PrimaryOf(view[r]) = r
           nl == Prefix(log[r], k)
       IN
       /\ (wasPrimary /\ ~MutPrimaryContinues) => view[r] + 1 <= MaxView
       /\ log' = [log EXCEPT ![r] = nl]
       /\ synced' = [synced EXCEPT ![r] = k]
       /\ commit' = [commit EXCEPT ![r] = 0]
       /\ dvcSent' = [dvcSent EXCEPT ![r] = FALSE]
       /\ fetch' = [fetch EXCEPT ![r] = None]
       /\ crashes' = crashes + 1
       /\ CASE current /\ ~wasPrimary ->
                 \* A backup of the current view is Normal.
                 /\ status' = [status EXCEPT ![r] = "Normal"]
                 /\ UNCHANGED <<view, svc, plogs>>
            [] wasPrimary /\ MutPrimaryContinues ->
                 \* Mutation (c): the primary continues with a shorter log.
                 /\ status' = [status EXCEPT ![r] = "Normal"]
                 /\ plogs' = [plogs EXCEPT ![view[r]] = @ \cup {nl}]
                 /\ UNCHANGED <<view, svc>>
            [] wasPrimary ->
                 \* The primary asks for view + 1.
                 /\ status' = [status EXCEPT ![r] = "ViewChange"]
                 /\ view' = [view EXCEPT ![r] = view[r] + 1]
                 /\ svc' = svc \cup {<<view[r] + 1, r>>}
                 /\ UNCHANGED plogs
            [] OTHER ->
                 \* logView < view: BeginViewChangeRound.
                 /\ status' = [status EXCEPT ![r] = "ViewChange"]
                 /\ svc' = svc \cup {<<view[r], r>>}
                 /\ UNCHANGED <<view, plogs>>
  /\ UNCHANGED <<logView, pcommit, vso, dvcs, acks, pcommitted>>

-----------------------------------------------------------------------------
\* View change.

\* MaybeSendDoViewChange: after StartViewChange from a quorum (with self).
\* The DoViewChange carries (log_view_, head op, head checksum, commit_max_).
\* The new primary keeps its own record locally.
SendDvc(r) ==
  /\ status[r] = "ViewChange" /\ ~dvcSent[r]
  /\ Cardinality({ s \in Replica : <<view[r], s>> \in svc } \cup {r}) >= Quorum
  /\ dvcSent' = [dvcSent EXCEPT ![r] = TRUE]
  /\ dvcs' = IF PrimaryOf(view[r]) = r THEN dvcs
             ELSE dvcs \cup {[v |-> view[r], src |-> r, lv |-> logView[r],
                              log |-> log[r], commit |-> commit[r]]}
  /\ UNCHANGED <<view, logView, log, synced, status, commit, fetch, plogs,
                 pcommit, vso, svc, acks, pcommitted, crashes>>

\* FinishViewChange: sync, persist log_view_ = view_, open the view.
OpenView(p, vc) ==
  LET v == view[p]
      c == Max(commit[p], Min(vc, Len(log[p])))
  IN /\ synced' = [synced EXCEPT ![p] = Len(log[p])]
     /\ logView' = [logView EXCEPT ![p] = v]
     /\ status' = [status EXCEPT ![p] = "Normal"]
     /\ commit' = [commit EXCEPT ![p] = c]
     /\ fetch' = [fetch EXCEPT ![p] = None]
     /\ plogs' = [plogs EXCEPT ![v] = @ \cup {log[p]}]
     /\ vso' = [vso EXCEPT ![v] = Len(log[p])]
     /\ pcommit' = [pcommit EXCEPT ![v] = Max(@, c)]

\* CheckViewChangeQuorum: the new primary takes DoViewChange messages from
\* a quorum (with its own record) and selects the best log. Any record that
\* no other record beats can be selected: this covers every tie order.
SelectLog(p) ==
  /\ status[p] = "ViewChange" /\ PrimaryOf(view[p]) = p
  /\ dvcSent[p] /\ fetch[p] = None
  /\ \E M \in SUBSET { m \in dvcs : m.v = view[p] /\ m.src # p } :
       LET own == [v |-> view[p], src |-> p, lv |-> logView[p],
                   log |-> log[p], commit |-> commit[p]]
           recs == M \cup {own}
           vc == CHOOSE c \in { m.commit : m \in recs } :
                   \A m \in recs : m.commit <= c
       IN /\ Cardinality({ m.src : m \in M }) = Cardinality(M)
          /\ Cardinality(M) + 1 >= Quorum
          /\ \E best \in recs :
               /\ \A m \in recs : ~Better(m, best)
               /\ IF best.log = log[p]
                    THEN /\ OpenView(p, vc)
                         /\ UNCHANGED <<view, dvcSent, svc, dvcs, acks,
                                        pcommitted, crashes, log>>
                    ELSE /\ fetch' = [fetch EXCEPT ![p] =
                                        [src |-> best.src, log |-> best.log, vc |-> vc]]
                         /\ UNCHANGED <<view, logView, log, synced, status, commit,
                                        dvcSent, plogs, pcommit, vso, svc, dvcs,
                                        acks, pcommitted, crashes>>

\* OnRequestPrepare: the source serves only while it is in ViewChange for
\* this view and has sent its DoViewChange. Its log does not change then.
Serving(p) ==
  LET s == fetch[p].src
  IN status[s] = "ViewChange" /\ view[s] = view[p] /\ dvcSent[s]

\* OnVerified while fetching: the commit numbers of the quorum apply.
FetchCommit(p, nl) ==
  commit' = [commit EXCEPT ![p] =
               IF IsPrefix(nl, fetch[p].log)
                 THEN Max(@, Min(fetch[p].vc, Len(fetch[p].log)))
                 ELSE @]

Fetching(p) ==
  /\ status[p] = "ViewChange" /\ PrimaryOf(view[p]) = p /\ fetch[p] # None

\* The new primary removes entries that are not in the selected log. It
\* keeps its old log_view_, and the truncation is durable.
FetchTruncate(p, k) ==
  LET T == fetch[p].log
  IN /\ Fetching(p)
     /\ k < Len(log[p])
     /\ k >= Min(commit[p], Len(log[p]))
     /\ \/ Serving(p) /\ Len(T) >= k + 1 /\ Prefix(T, k + 1) # Prefix(log[p], k + 1)
        \* Follow: compare with the target head only.
        \/ Len(log[p]) >= Len(T) /\ k = Len(T) /\ Prefix(log[p], k) = T
        \/ Len(log[p]) >= Len(T) /\ k + 1 = Len(T) /\ Prefix(log[p], k + 1) # T
     /\ log' = [log EXCEPT ![p] = Prefix(log[p], k)]
     /\ synced' = [synced EXCEPT ![p] = k]
     /\ FetchCommit(p, Prefix(log[p], k))
     /\ UNCHANGED <<view, logView, status, dvcSent, fetch, plogs, pcommit, vso,
                    svc, dvcs, acks, pcommitted, crashes>>

\* The new primary appends the next entry of the selected log (not synced).
FetchAppend(p) ==
  LET T == fetch[p].log
  IN /\ Fetching(p) /\ Serving(p)
     /\ Len(log[p]) < Len(T) /\ IsPrefix(log[p], T)
     /\ log' = [log EXCEPT ![p] = Append(log[p], T[Len(log[p]) + 1])]
     /\ FetchCommit(p, log'[p])
     /\ UNCHANGED <<view, logView, synced, status, dvcSent, fetch, plogs,
                    pcommit, vso, svc, dvcs, acks, pcommitted, crashes>>

\* CheckFollowDone while fetching: the log equals the selected log.
FetchFinish(p) ==
  /\ Fetching(p)
  /\ log[p] = fetch[p].log
  /\ OpenView(p, fetch[p].vc)
  /\ UNCHANGED <<view, log, dvcSent, svc, dvcs, acks, pcommitted, crashes>>

-----------------------------------------------------------------------------
Step ==
  \/ \E p \in Replica, x \in Value : ClientRequest(p, x)
  \/ \E p \in Replica : PrimaryCommit(p)
  \/ \E r \in Replica : Ack(r)
  \/ \E r \in Replica, v \in Views : LearnView(r, v)
  \/ \E r \in Replica, k \in 0 .. MaxLog : JoinTruncate(r, k)
  \/ \E r \in Replica : JoinAppend(r)
  \/ \E r \in Replica : JoinFinish(r)
  \/ \E r \in Replica, c \in 1 .. MaxLog : LearnCommit(r, c)
  \/ \E r \in Replica : Sync(r)
  \/ \E r \in Replica, v \in Views : StartViewChange(r, v)
  \/ \E r \in Replica : Crash(r)
  \/ \E r \in Replica : SendDvc(r)
  \/ \E p \in Replica : SelectLog(p)
  \/ \E p \in Replica, k \in 0 .. MaxLog : FetchTruncate(p, k)
  \/ \E p \in Replica : FetchAppend(p)
  \/ \E p \in Replica : FetchFinish(p)

\* The ghost set `committed` collects every entry that any replica executes
\* (every op up to min(commit_max_, head)), in every state.
Next ==
  /\ Step
  /\ committed' = committed \cup CommittedNow'

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Properties.

TypeOK ==
  /\ view \in [Replica -> Views]
  /\ logView \in [Replica -> Views]
  /\ \A r \in Replica : /\ log[r] \in Seq(Entry) /\ Len(log[r]) <= MaxLog
                        /\ synced[r] \in 0 .. Len(log[r])
                        /\ logView[r] <= view[r]
  /\ status \in [Replica -> Status]
  /\ commit \in [Replica -> 0 .. MaxLog]

\* Agreement. Two replicas never take different entries as committed at
\* the same op. `committed` never shrinks, so a committed entry is never
\* replaced at its op either.
Agreement ==
  \A a, b \in committed : a[1] = b[1] => a[2] = b[2]

\* Every committed entry must remain on a durable majority.
DurableCommit ==
  \A c \in committed :
    Cardinality({r \in Replica :
      /\ synced[r] >= c[1]
      /\ log[r][c[1]] = c[2]}) >= Quorum

\* Replica::BecomeNormal: a durable log_view_ = v means that the durable
\* log holds every entry that a primary committed in a view before v.
LogViewInvariant ==
  \A r \in Replica : \A c \in pcommitted :
     c[3] < logView[r] => Len(DurableLog(r)) >= c[1] /\ DurableLog(r)[c[1]] = c[2]

\* Each view has at most one Normal primary. The primary index is fixed.
OnePrimaryPerView ==
  /\ \A r1, r2 \in Replica :
       (r1 # r2 /\ status[r1] = "Normal" /\ status[r2] = "Normal"
        /\ view[r1] = view[r2]) => ~(PrimaryOf(view[r1]) = r1 /\ PrimaryOf(view[r2]) = r2)

\* TLC: the client values are interchangeable.
ValueSymmetry == Permutations(Value)
=============================================================================
