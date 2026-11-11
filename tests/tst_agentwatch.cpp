#include "agentwatch.h"

#include <QTest>

using namespace df;

// The service keeps an agent (the owner of the user's clipboard on Windows) running in the console
// session, with a grace period and exponential back-off.
class TestAgentWatch : public QObject
{
    Q_OBJECT
private slots:
    void missingAgentIsRelaunchedWithBackoff()
    {
        qint64 t = 1000;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        watch.setUserPresent(true);

        watch.check();
        QCOMPARE(launches, 0); // the grace period: it may be starting
        t += AgentWatch::kGraceMs;
        watch.check();
        QCOMPARE(launches, 1);

        // Still missing: retried after the first delay, then after twice as long.
        t += AgentWatch::kFirstRetryMs - 1;
        watch.check();
        QCOMPARE(launches, 1);
        t += 1;
        watch.check();
        QCOMPARE(launches, 2);
        t += AgentWatch::kFirstRetryMs;
        watch.check();
        QCOMPARE(launches, 2);
        t += AgentWatch::kFirstRetryMs;
        watch.check();
        QCOMPARE(launches, 3);

        // The delay never exceeds the maximum.
        for (int i = 0; i < 20; ++i) {
            t += AgentWatch::kMaxRetryMs;
            watch.check();
        }
        QCOMPARE(launches, 23);
    }

    void connectedAgentIsLeftAlone()
    {
        qint64 t = 0;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        watch.setUserPresent(true);
        watch.setAgentConnected(true);
        t += 10 * AgentWatch::kMaxRetryMs;
        watch.check();
        QCOMPARE(launches, 0);
    }

    void noUserNoRelaunch()
    {
        qint64 t = 0;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        t += 10 * AgentWatch::kGraceMs;
        watch.check();
        QCOMPARE(launches, 0); // nobody signed in: it starts at the next sign-in
    }

    // With autostart off the agent is restarted only if it ran in this session (it crashed); a
    // user who never started it is left alone.
    void autostartOffRelaunchesOnlyALostAgent()
    {
        qint64 t = 0;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        watch.setAutostart(false);
        watch.setUserPresent(true);
        t += 10 * AgentWatch::kGraceMs;
        watch.check();
        watch.check();
        QCOMPARE(launches, 0);

        watch.setAgentConnected(true);
        t += 1000;
        watch.setAgentConnected(false); // it died
        watch.check();
        t += AgentWatch::kGraceMs;
        watch.check();
        QCOMPARE(launches, 1);

        // Another user signs in: what ran for the previous one does not count.
        watch.userChanged();
        t += 10 * AgentWatch::kMaxRetryMs;
        watch.check();
        watch.check();
        QCOMPARE(launches, 1);
    }

    // Right after a sign-in the agent the Run key starts (late) is waited for, so the service does
    // not start a second one; once that time is over a missing agent is started.
    void signInWaitsForTheAutostartedAgent()
    {
        qint64 t = 0;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        watch.userChanged(); // a user signs in...
        watch.setUserPresent(true); // ...and the session helper sees them
        for (qint64 elapsed = 0; elapsed < AgentWatch::kSignInGraceMs; elapsed += AgentWatch::kCheckIntervalMs) {
            watch.check();
            t += AgentWatch::kCheckIntervalMs;
        }
        QCOMPARE(launches, 0);
        watch.check();
        QCOMPARE(launches, 1);

        // The autostarted agent arriving within that time is all there is.
        watch.setAgentConnected(true);
        watch.userChanged();
        watch.setAgentConnected(false);
        t += AgentWatch::kSignInGraceMs / 2;
        watch.check();
        watch.setAgentConnected(true);
        t += AgentWatch::kSignInGraceMs;
        watch.check();
        QCOMPARE(launches, 1);
    }

    // An agent that stays up resets the back-off: a later loss is retried soon again.
    void stableAgentResetsTheBackoff()
    {
        qint64 t = 0;
        int launches = 0;
        AgentWatch watch([&] { ++launches; }, [&] { return t; });
        watch.setUserPresent(true);
        watch.check(); // missing from now on
        for (int i = 0; i < 6; ++i) { // grow the delay
            t += AgentWatch::kMaxRetryMs;
            watch.check();
        }
        QCOMPARE(launches, 6);
        watch.setAgentConnected(true);
        t += AgentWatch::kStableMs;
        watch.check();
        watch.setAgentConnected(false);
        watch.check();
        t += AgentWatch::kGraceMs;
        watch.check();
        QCOMPARE(launches, 7);
        t += AgentWatch::kFirstRetryMs;
        watch.check();
        QCOMPARE(launches, 8); // the first delay again, not the grown one
    }
};

QTEST_GUILESS_MAIN(TestAgentWatch)
#include "tst_agentwatch.moc"
