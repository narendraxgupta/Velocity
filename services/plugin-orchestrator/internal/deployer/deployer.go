// Package deployer turns a parsed plugin Manifest into Kubernetes
// objects (Deployment + Service) inside the tenant's namespace.
//
// Why Deployment, not Pod?
// ------------------------
//   - Deployment gives us free restart-on-crash via the controller.
//   - Service gives us stable DNS that the correctness-validator can
//     dial without watching pod IPs.
//   - We could use a single Pod with restartPolicy=Always but it would
//     mean writing the watcher ourselves.
//
// Security hardening:
//   - Runs as non-root, read-only root FS, all capabilities dropped.
//   - The pod-security label on tenant namespaces is "restricted", so
//     anything the user image tries to escalate to is rejected at admit
//     time.
//   - NetworkPolicy (see infra/kubernetes/base/tenants/tenant-template
//     .yaml) blocks egress except to the data plane; the plugin can
//     reach gRPC but not the public Internet.
//
// Idempotency:
//   - Apply is upsert-shaped. We compute the desired object set, fetch
//     the existing set, and reconcile by name. A second invocation with
//     the same manifest is a no-op.
package deployer

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/client-go/kubernetes"

	"github.com/velocity/platform/services/plugin-orchestrator/internal/registry"
)

type Deployer struct {
	k8s kubernetes.Interface
}

func New(k kubernetes.Interface) *Deployer { return &Deployer{k8s: k} }

// Apply reconciles the cluster state to match the manifest. This is a true
// reconcile: enabled plugins are upserted AND plugins that were removed from
// the manifest (or flipped to disabled) are pruned. Without the prune step a
// disabled/removed plugin kept running indefinitely — consuming the tenant's
// quota and, worse, leaving an unmanaged workload reachable on the data plane.
func (d *Deployer) Apply(ctx context.Context, m *registry.Manifest) error {
	namespace := "velocity-tenant-" + m.Tenant
	desired := make(map[string]struct{}, len(m.Plugins))
	for i := range m.Plugins {
		p := m.Plugins[i]
		if !p.Enabled {
			continue
		}
		name := suffixHashed(p.ID, registry.ResourceName(m.Tenant, p.ID))
		desired[name] = struct{}{}
		if err := d.applyDeployment(ctx, namespace, name, &p); err != nil {
			return fmt.Errorf("apply deployment %s: %w", name, err)
		}
		if err := d.applyService(ctx, namespace, name, &p); err != nil {
			return fmt.Errorf("apply service %s: %w", name, err)
		}
	}
	if err := d.prune(ctx, namespace, desired); err != nil {
		return fmt.Errorf("prune stale plugins: %w", err)
	}
	return nil
}

// prune deletes velocity-managed plugin Deployments/Services in the namespace
// that are not in the desired set. Deployment and Service share a name, so we
// reconcile each kind independently to also catch a half-created pair.
func (d *Deployer) prune(ctx context.Context, ns string, desired map[string]struct{}) error {
	const selector = "app.kubernetes.io/name=velocity-plugin,app.kubernetes.io/part-of=velocity"

	deps, err := d.k8s.AppsV1().Deployments(ns).List(ctx, metav1.ListOptions{LabelSelector: selector})
	if err != nil {
		return fmt.Errorf("list deployments: %w", err)
	}
	for i := range deps.Items {
		name := deps.Items[i].Name
		if _, keep := desired[name]; keep {
			continue
		}
		if err := d.k8s.AppsV1().Deployments(ns).Delete(ctx, name, metav1.DeleteOptions{}); err != nil &&
			!apierrors.IsNotFound(err) {
			return fmt.Errorf("delete deployment %s: %w", name, err)
		}
	}

	svcs, err := d.k8s.CoreV1().Services(ns).List(ctx, metav1.ListOptions{LabelSelector: selector})
	if err != nil {
		return fmt.Errorf("list services: %w", err)
	}
	for i := range svcs.Items {
		name := svcs.Items[i].Name
		if _, keep := desired[name]; keep {
			continue
		}
		if err := d.k8s.CoreV1().Services(ns).Delete(ctx, name, metav1.DeleteOptions{}); err != nil &&
			!apierrors.IsNotFound(err) {
			return fmt.Errorf("delete service %s: %w", name, err)
		}
	}
	return nil
}

// Suffix the resource name with a 6-char hash so that pathological id
// collisions after truncation don't accidentally fuse two plugins.
func suffixHashed(id, base string) string {
	h := sha256.Sum256([]byte(id))
	return fmt.Sprintf("%s-%s", base, hex.EncodeToString(h[:3]))
}

func (d *Deployer) applyDeployment(ctx context.Context, ns, name string, p *registry.Plugin) error {
	labels := map[string]string{
		"app.kubernetes.io/name":     "velocity-plugin",
		"velocity.plugin.id":         sanitiseLabel(p.ID),
		"app.kubernetes.io/part-of":  "velocity",
	}
	cpu, err := resource.ParseQuantity(coalesce(p.CPU, "500m"))
	if err != nil {
		return fmt.Errorf("invalid cpu: %w", err)
	}
	mem, err := resource.ParseQuantity(coalesce(p.Memory, "256Mi"))
	if err != nil {
		return fmt.Errorf("invalid memory: %w", err)
	}

	env := make([]corev1.EnvVar, 0, len(p.Env))
	for k, v := range p.Env {
		env = append(env, corev1.EnvVar{Name: k, Value: v})
	}

	replicas := int32(1)
	dep := &appsv1.Deployment{
		ObjectMeta: metav1.ObjectMeta{Name: name, Namespace: ns, Labels: labels},
		Spec: appsv1.DeploymentSpec{
			Replicas: &replicas,
			Strategy: appsv1.DeploymentStrategy{Type: appsv1.RecreateDeploymentStrategyType},
			Selector: &metav1.LabelSelector{MatchLabels: labels},
			Template: corev1.PodTemplateSpec{
				ObjectMeta: metav1.ObjectMeta{Labels: labels},
				Spec: corev1.PodSpec{
					AutomountServiceAccountToken: ptrBool(false),
					SecurityContext: &corev1.PodSecurityContext{
						RunAsNonRoot:   ptrBool(true),
						RunAsUser:      ptrInt64(65532),
						SeccompProfile: &corev1.SeccompProfile{Type: corev1.SeccompProfileTypeRuntimeDefault},
					},
					Containers: []corev1.Container{{
						Name:  "plugin",
						Image: p.Image,
						Ports: []corev1.ContainerPort{{Name: "grpc", ContainerPort: p.Port}},
						Env:   env,
						Resources: corev1.ResourceRequirements{
							Requests: corev1.ResourceList{
								corev1.ResourceCPU:    cpu,
								corev1.ResourceMemory: mem,
							},
							Limits: corev1.ResourceList{
								corev1.ResourceCPU:    cpu,
								corev1.ResourceMemory: mem,
							},
						},
						SecurityContext: &corev1.SecurityContext{
							AllowPrivilegeEscalation: ptrBool(false),
							ReadOnlyRootFilesystem:   ptrBool(true),
							Capabilities:             &corev1.Capabilities{Drop: []corev1.Capability{"ALL"}},
						},
						LivenessProbe: &corev1.Probe{
							ProbeHandler: corev1.ProbeHandler{
								TCPSocket: &corev1.TCPSocketAction{Port: intstr.FromString("grpc")},
							},
							InitialDelaySeconds: 5,
							PeriodSeconds:       10,
						},
					}},
				},
			},
		},
	}

	existing, err := d.k8s.AppsV1().Deployments(ns).Get(ctx, name, metav1.GetOptions{})
	if apierrors.IsNotFound(err) {
		_, err = d.k8s.AppsV1().Deployments(ns).Create(ctx, dep, metav1.CreateOptions{})
		return err
	} else if err != nil {
		return err
	}
	// Preserve resource version so the optimistic update succeeds.
	dep.ResourceVersion = existing.ResourceVersion
	_, err = d.k8s.AppsV1().Deployments(ns).Update(ctx, dep, metav1.UpdateOptions{})
	return err
}

func (d *Deployer) applyService(ctx context.Context, ns, name string, p *registry.Plugin) error {
	svc := &corev1.Service{
		ObjectMeta: metav1.ObjectMeta{
			Name: name, Namespace: ns,
			Labels: map[string]string{
				"app.kubernetes.io/name":    "velocity-plugin",
				"velocity.plugin.id":        sanitiseLabel(p.ID),
				"app.kubernetes.io/part-of": "velocity",
			},
		},
		Spec: corev1.ServiceSpec{
			Selector: map[string]string{"velocity.plugin.id": sanitiseLabel(p.ID)},
			Ports: []corev1.ServicePort{{
				Name: "grpc", Port: p.Port, TargetPort: intstr.FromString("grpc"),
			}},
		},
	}
	existing, err := d.k8s.CoreV1().Services(ns).Get(ctx, name, metav1.GetOptions{})
	if apierrors.IsNotFound(err) {
		_, err = d.k8s.CoreV1().Services(ns).Create(ctx, svc, metav1.CreateOptions{})
		return err
	} else if err != nil {
		return err
	}
	// Re-applying a Service requires preserving ClusterIP — it's
	// immutable post-creation.
	svc.Spec.ClusterIP = existing.Spec.ClusterIP
	svc.ResourceVersion = existing.ResourceVersion
	_, err = d.k8s.CoreV1().Services(ns).Update(ctx, svc, metav1.UpdateOptions{})
	return err
}

func sanitiseLabel(id string) string {
	// Plugin IDs may contain '/' and '.', neither valid in a label
	// value. Replace with '-' and trim. The validator + UI keep using
	// the original id; this is just for the K8s selector.
	out := make([]byte, 0, len(id))
	for _, c := range id {
		switch {
		case c >= 'a' && c <= 'z', c >= '0' && c <= '9', c == '-':
			out = append(out, byte(c))
		case c == '/', c == '.':
			out = append(out, '-')
		}
	}
	if len(out) > 63 {
		out = out[:63]
	}
	return string(out)
}

func coalesce(a, b string) string {
	if a == "" {
		return b
	}
	return a
}

func ptrBool(b bool) *bool   { return &b }
func ptrInt64(n int64) *int64 { return &n }
